/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mirrors the old branch tests, comparing our tracked total against
 * objectComputeSize. Logs the gap (jemalloc rounding) for each test.
 */

#include "generated_wrappers.hpp"

#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstring>

extern "C" {
#include "listpack.h"
#include "quicklist.h"
#include "server.h"
#include "zmalloc.h"

size_t objectComputeSize(robj *key, robj *o, size_t sample_size, int dbid);
}

class QuicklistTrackingVsComputeTest : public ::testing::Test {
};

static size_t trackedTotal(quicklist *ql) {
    return sizeof(quicklist) + ql->len * sizeof(quicklistNode) + ql->tracked_data_bytes;
}

/* Compare tracked total against objectComputeSize. Log the gap. */
#define LOG_TRACKED_VS_COMPUTE(list)                                                    \
    do {                                                                                \
        quicklist *_ql = (quicklist *)objectGetVal(list);                               \
        if (_ql->len > 0) {                                                             \
            size_t _computed = objectComputeSize(nullptr, (list), SIZE_MAX, 0);         \
            size_t _tracked = trackedTotal(_ql);                                        \
            long long _gap = (long long)_computed - (long long)_tracked;                \
            double _pct = _computed ? 100.0 * _gap / _computed : 0;                     \
            printf("  computed=%zu tracked=%zu gap=%lld (%.1f%%)\n",                     \
                   _computed, _tracked, _gap, _pct);                                    \
            ASSERT_EQ(_computed, _tracked);                                             \
        } else {                                                                        \
            ASSERT_EQ(_ql->tracked_data_bytes, 0ul);                                    \
        }                                                                               \
    } while (0)

/* 1 */
TEST_F(QuicklistTrackingVsComputeTest, BasicPushHeadTail) {
    robj *list = createQuicklistObject(3, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 50; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "element_%d", i);
        if (i % 2 == 0) quicklistPushTail(ql, buf, strlen(buf));
        else quicklistPushHead(ql, buf, strlen(buf));
    }
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 2 */
TEST_F(QuicklistTrackingVsComputeTest, PushCreatesNewNodes) {
    robj *list = createQuicklistObject(1, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 20; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "single_node_element_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 3 */
TEST_F(QuicklistTrackingVsComputeTest, PopHeadAndTail) {
    robj *list = createQuicklistObject(3, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 30; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "pop_test_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    LOG_TRACKED_VS_COMPUTE(list);
    for (int i = 0; i < 8; i++) {
        unsigned char *data; size_t sz; long long val;
        quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
        if (data) zfree(data);
    }
    LOG_TRACKED_VS_COMPUTE(list);
    for (int i = 0; i < 8; i++) {
        unsigned char *data; size_t sz; long long val;
        quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &val);
        if (data) zfree(data);
    }
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 4 */
TEST_F(QuicklistTrackingVsComputeTest, DeleteEntireNode) {
    robj *list = createQuicklistObject(2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "del_node_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    LOG_TRACKED_VS_COMPUTE(list);
    while (quicklistCount(ql) > 0) {
        unsigned char *data; size_t sz; long long val;
        quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
        if (data) zfree(data);
        LOG_TRACKED_VS_COMPUTE(list);
    }
    decrRefCount(list);
}

/* 5 */
TEST_F(QuicklistTrackingVsComputeTest, CompressDecompress) {
    robj *list = createQuicklistObject(-2, 1);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 500; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "compress_test_value_%d_padding_data", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    LOG_TRACKED_VS_COMPUTE(list);
    for (int i = 0; i < 50; i++) {
        unsigned char *data; size_t sz; long long val;
        quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
        if (data) zfree(data);
    }
    LOG_TRACKED_VS_COMPUTE(list);
    for (int i = 0; i < 50; i++) {
        unsigned char *data; size_t sz; long long val;
        quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &val);
        if (data) zfree(data);
    }
    LOG_TRACKED_VS_COMPUTE(list);
    for (int i = 0; i < 50; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "after_compress_push_%d", i);
        quicklistPushHead(ql, buf, strlen(buf));
    }
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 6 */
TEST_F(QuicklistTrackingVsComputeTest, InsertNonFullNode) {
    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 20; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "insert_test_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    LOG_TRACKED_VS_COMPUTE(list);
    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 5, &entry);
    quicklistInsertAfter(iter, &entry, (void *)"INSERTED_AFTER", 14);
    quicklistReleaseIterator(iter);
    LOG_TRACKED_VS_COMPUTE(list);
    iter = quicklistGetIteratorEntryAtIdx(ql, 10, &entry);
    quicklistInsertBefore(iter, &entry, (void *)"INSERTED_BEFORE", 15);
    quicklistReleaseIterator(iter);
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 7 */
TEST_F(QuicklistTrackingVsComputeTest, InsertTriggersSplit) {
    robj *list = createQuicklistObject(3, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 12; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "split_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    LOG_TRACKED_VS_COMPUTE(list);
    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 4, &entry);
    quicklistInsertAfter(iter, &entry, (void *)"SPLIT_INSERT", 12);
    quicklistReleaseIterator(iter);
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 8a */
TEST_F(QuicklistTrackingVsComputeTest, InsertNextNeighbor) {
    robj *list = createQuicklistObject(4, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 6; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "neighbor_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 3, &entry);
    quicklistInsertAfter(iter, &entry, (void *)"NEIGHBOR_NEXT", 13);
    quicklistReleaseIterator(iter);
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 8b */
TEST_F(QuicklistTrackingVsComputeTest, InsertPrevNeighbor) {
    robj *list = createQuicklistObject(4, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 8; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "avprev_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    unsigned char *data; size_t sz; long long val;
    quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
    if (data) zfree(data);
    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 3, &entry);
    quicklistInsertBefore(iter, &entry, (void *)"NEIGHBOR_PREV", 13);
    quicklistReleaseIterator(iter);
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 9 */
TEST_F(QuicklistTrackingVsComputeTest, ReplaceEntryListpack) {
    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 20; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "replace_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 5, &entry);
    quicklistReplaceEntry(iter, &entry, (void *)"S", 1);
    quicklistReleaseIterator(iter);
    LOG_TRACKED_VS_COMPUTE(list);
    iter = quicklistGetIteratorEntryAtIdx(ql, 10, &entry);
    char big[200];
    memset(big, 'X', sizeof(big));
    quicklistReplaceEntry(iter, &entry, big, sizeof(big));
    quicklistReleaseIterator(iter);
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 10 */
TEST_F(QuicklistTrackingVsComputeTest, ReplaceEntryPlainNode) {
    quicklistSetPackedThreshold(64);
    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    char large[128];
    memset(large, 'A', sizeof(large));
    quicklistPushTail(ql, large, sizeof(large));
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "mixed_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
    char new_large[256];
    memset(new_large, 'B', sizeof(new_large));
    quicklistReplaceEntry(iter, &entry, new_large, sizeof(new_large));
    quicklistReleaseIterator(iter);
    LOG_TRACKED_VS_COMPUTE(list);
    quicklistSetPackedThreshold(0);
    decrRefCount(list);
}

/* 11 */
TEST_F(QuicklistTrackingVsComputeTest, DelRange) {
    robj *list = createQuicklistObject(5, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 30; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "delrange_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    quicklistDelRange(ql, 3, 2);
    LOG_TRACKED_VS_COMPUTE(list);
    quicklistDelRange(ql, 2, 15);
    LOG_TRACKED_VS_COMPUTE(list);
    quicklistDelRange(ql, 0, 3);
    LOG_TRACKED_VS_COMPUTE(list);
    quicklistDelRange(ql, -3, 3);
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 12 */
TEST_F(QuicklistTrackingVsComputeTest, Rotate) {
    robj *list = createQuicklistObject(3, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 15; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "rotate_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    for (int i = 0; i < 10; i++) {
        quicklistRotate(ql);
    }
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 13 */
TEST_F(QuicklistTrackingVsComputeTest, Dup) {
    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 100; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "dup_test_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    LOG_TRACKED_VS_COMPUTE(list);
    quicklist *copy = quicklistDup(ql);
    robj *copy_list = createObject(OBJ_LIST, copy);
    copy_list->encoding = OBJ_ENCODING_QUICKLIST;
    LOG_TRACKED_VS_COMPUTE(copy_list);
    decrRefCount(copy_list);
    decrRefCount(list);
}

/* 14 */
TEST_F(QuicklistTrackingVsComputeTest, DupWithCompression) {
    robj *list = createQuicklistObject(-2, 1);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 500; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "dup_compress_%d_padding_data_here", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    LOG_TRACKED_VS_COMPUTE(list);
    quicklist *copy = quicklistDup(ql);
    robj *copy_list = createObject(OBJ_LIST, copy);
    copy_list->encoding = OBJ_ENCODING_QUICKLIST;
    LOG_TRACKED_VS_COMPUTE(copy_list);
    decrRefCount(copy_list);
    decrRefCount(list);
}

/* 15 */
TEST_F(QuicklistTrackingVsComputeTest, NodeMerge) {
    robj *list = createQuicklistObject(4, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 12; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "merge_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    quicklistDelRange(ql, 4, 3);
    quicklistDelRange(ql, 5, 3);
    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 2, &entry);
    quicklistInsertAfter(iter, &entry, (void *)"MERGE_TRIGGER", 13);
    quicklistReleaseIterator(iter);
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 16 */
TEST_F(QuicklistTrackingVsComputeTest, PlainNodeInsert) {
    quicklistSetPackedThreshold(64);
    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "small_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    for (int i = 0; i < 5; i++) {
        char large[128];
        memset(large, 'P' + i, sizeof(large));
        quicklistPushTail(ql, large, sizeof(large));
    }
    LOG_TRACKED_VS_COMPUTE(list);
    for (int i = 0; i < 3; i++) {
        unsigned char *data; size_t sz; long long val;
        quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &val);
        if (data) zfree(data);
    }
    LOG_TRACKED_VS_COMPUTE(list);
    quicklistSetPackedThreshold(0);
    decrRefCount(list);
}

/* 17 */
TEST_F(QuicklistTrackingVsComputeTest, InsertEmptyList) {
    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    quicklistEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.quicklist = ql;
    quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
    quicklistInsertAfter(iter, &entry, (void *)"first_ever", 10);
    quicklistReleaseIterator(iter);
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 18 */
TEST_F(QuicklistTrackingVsComputeTest, InterleavedCompressOps) {
    robj *list = createQuicklistObject(-2, 2);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 1000; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "interleaved_%d_extra_padding_here", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    for (int i = 0; i < 100; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "interleaved_new_%d", i);
        if (i % 2 == 0) quicklistPushHead(ql, buf, strlen(buf));
        else quicklistPushTail(ql, buf, strlen(buf));
        unsigned char *data; size_t sz; long long val;
        if (i % 3 == 0) quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
        else quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &val);
        if (data) zfree(data);
    }
    LOG_TRACKED_VS_COMPUTE(list);
    for (int i = 0; i < 10; i++) {
        quicklistEntry entry;
        quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, (long)quicklistCount(ql) / 2, &entry);
        if (iter) {
            char buf[100];
            memset(buf, 'R', sizeof(buf));
            quicklistReplaceEntry(iter, &entry, buf, sizeof(buf));
            quicklistReleaseIterator(iter);
        }
    }
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 19 */
TEST_F(QuicklistTrackingVsComputeTest, AppendListpackAndPlainNode) {
    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    unsigned char *lp = lpNew(0);
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "rdb_lp_%d", i);
        lp = lpAppend(lp, (unsigned char *)buf, strlen(buf));
    }
    quicklistAppendListpack(ql, lp);
    LOG_TRACKED_VS_COMPUTE(list);
    size_t plain_sz = 256;
    unsigned char *plain_data = (unsigned char *)zmalloc(plain_sz);
    memset(plain_data, 'Z', plain_sz);
    quicklistAppendPlainNode(ql, plain_data, plain_sz);
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 20 */
TEST_F(QuicklistTrackingVsComputeTest, DelEntryViaIterator) {
    robj *list = createQuicklistObject(5, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 25; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "delentry_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
    quicklistEntry entry;
    int count = 0;
    while (quicklistNext(iter, &entry)) {
        if (count % 2 == 0) quicklistDelEntry(iter, &entry);
        count++;
    }
    quicklistReleaseIterator(iter);
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 21 */
TEST_F(QuicklistTrackingVsComputeTest, ReplaceAtIndex) {
    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 30; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "replaceatidx_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    quicklistReplaceAtIndex(ql, 0, (void *)"HEAD_REPLACED", 13);
    quicklistReplaceAtIndex(ql, 15, (void *)"MID_REPLACED", 12);
    quicklistReplaceAtIndex(ql, 29, (void *)"TAIL_REPLACED", 13);
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 22 */
TEST_F(QuicklistTrackingVsComputeTest, SameSizeReplace) {
    robj *list = createQuicklistObject(4, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 8; i++) {
        quicklistPushTail(ql, (void *)"aaaa", 4);
    }
    quicklistReplaceAtIndex(ql, 0, (void *)"bbbbbbbbbbbbbbbb", 16);
    quicklistReplaceAtIndex(ql, 7, (void *)"cccc", 4);
    quicklistReplaceAtIndex(ql, 5, (void *)"dddd", 4);
    quicklistReplaceAtIndex(ql, 6, (void *)"eeee", 4);
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 23. Empty after all pops */
TEST_F(QuicklistTrackingVsComputeTest, EmptyAfterPops) {
    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 5; i++) {
        quicklistPushTail(ql, (void *)"hello", 5);
    }
    while (quicklistCount(ql) > 0) {
        unsigned char *data; size_t sz; long long val;
        quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
        if (data) zfree(data);
    }
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 24. Replace plain node with small value (plain→packed conversion) */
TEST_F(QuicklistTrackingVsComputeTest, ReplacePlainWithSmall) {
    quicklistSetPackedThreshold(64);
    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    char large[128];
    memset(large, 'A', sizeof(large));
    quicklistPushTail(ql, large, sizeof(large));
    quicklistPushTail(ql, (void *)"small", 5);
    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
    quicklistReplaceEntry(iter, &entry, (void *)"tiny", 4);
    quicklistReleaseIterator(iter);
    LOG_TRACKED_VS_COMPUTE(list);
    quicklistSetPackedThreshold(0);
    decrRefCount(list);
}

/* 25. Large element insert with split */
TEST_F(QuicklistTrackingVsComputeTest, LargeElementInsertWithSplit) {
    quicklistSetPackedThreshold(64);
    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 20; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "item_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 10, &entry);
    char large[128];
    memset(large, 'L', sizeof(large));
    quicklistInsertAfter(iter, &entry, large, sizeof(large));
    quicklistReleaseIterator(iter);
    LOG_TRACKED_VS_COMPUTE(list);
    quicklistSetPackedThreshold(0);
    decrRefCount(list);
}

/* 26. Deep compress depth */
TEST_F(QuicklistTrackingVsComputeTest, DeepCompressDepth) {
    robj *list = createQuicklistObject(-2, 3);
    quicklist *ql = (quicklist *)objectGetVal(list);
    for (int i = 0; i < 2000; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "deep_compress_%d_padding_data_xxxx", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    LOG_TRACKED_VS_COMPUTE(list);
    for (int i = 0; i < 200; i++) {
        unsigned char *data; size_t sz; long long val;
        if (i % 2 == 0) quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
        else quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &val);
        if (data) zfree(data);
    }
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}

/* 27. Fuzzer */
TEST_F(QuicklistTrackingVsComputeTest, Fuzzer) {
    unsigned int seed = 42;
    robj *list = createQuicklistObject(-2, 1);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 100; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "seed_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }

    /* Weighted ops: ~60% adds, ~25% removes, ~15% replaces.
     * 0-3: pushHead/pushTail/insertAfter/pushTail (adds)
     * 4-5: popHead/popTail (removes)
     * 6: delRange (remove)
     * 7-8: replaceAtIndex (mutate)
     * 9: rotate (structural) */
    for (int i = 0; i < 2000; i++) {
        int op = rand_r(&seed) % 10;
        char buf[64];
        snprintf(buf, sizeof(buf), "fuzz_%d_%d", op, i);
        size_t len = strlen(buf);

        switch (op) {
        case 0:
            quicklistPushHead(ql, buf, len);
            break;
        case 1:
        case 3:
            quicklistPushTail(ql, buf, len);
            break;
        case 2:
            if (quicklistCount(ql) > 0) {
                long idx = rand_r(&seed) % quicklistCount(ql);
                quicklistEntry entry;
                quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, idx, &entry);
                if (iter) {
                    quicklistInsertAfter(iter, &entry, buf, len);
                    quicklistReleaseIterator(iter);
                }
            } else {
                quicklistPushTail(ql, buf, len);
            }
            break;
        case 4:
            if (quicklistCount(ql) > 0) {
                unsigned char *data; size_t sz; long long val;
                quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
                if (data) zfree(data);
            }
            break;
        case 5:
            if (quicklistCount(ql) > 0) {
                unsigned char *data; size_t sz; long long val;
                quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &val);
                if (data) zfree(data);
            }
            break;
        case 6:
            if (quicklistCount(ql) > 3) {
                long idx = rand_r(&seed) % quicklistCount(ql);
                quicklistDelRange(ql, idx, 1);
            }
            break;
        case 7:
        case 8:
            if (quicklistCount(ql) > 0) {
                long idx = rand_r(&seed) % quicklistCount(ql);
                quicklistReplaceAtIndex(ql, idx, buf, len);
            }
            break;
        case 9:
            if (quicklistCount(ql) > 0) {
                quicklistRotate(ql);
            }
            break;
        }
    }
    LOG_TRACKED_VS_COMPUTE(list);
    decrRefCount(list);
}
