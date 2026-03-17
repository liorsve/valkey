/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Correctness tests for quicklist tracked_data_bytes.
 * Verifies tracking by walking the list and summing node entry sizes.
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
}

class QuicklistTrackingTest : public ::testing::Test {
};

/* Walk all nodes and sum the logical entry sizes — the ground truth. */
static size_t computeExpectedDataBytes(quicklist *ql) {
    size_t total = 0;
    for (quicklistNode *node = ql->head; node; node = node->next) {
        if (node->encoding == QUICKLIST_NODE_ENCODING_LZF) {
            quicklistLZF *lzf = (quicklistLZF *)node->entry;
            total += sizeof(*lzf) + lzf->sz;
        } else {
            total += node->sz;
        }
    }
    return total;
}

#define ASSERT_TRACKED_CORRECT(ql)                                             \
    do {                                                                       \
        size_t _expected = computeExpectedDataBytes(ql);                       \
        ASSERT_EQ((ql)->tracked_data_bytes, _expected);                        \
    } while (0)

/* 1. Basic pushHead / pushTail into existing nodes */
TEST_F(QuicklistTrackingTest, BasicPushHeadTail) {
    quicklist *ql = quicklistNew(3, 0);

    for (int i = 0; i < 50; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "element_%d", i);
        if (i % 2 == 0)
            quicklistPushTail(ql, buf, strlen(buf));
        else
            quicklistPushHead(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_CORRECT(ql);
    quicklistRelease(ql);
}

/* 2. pushHead / pushTail that create new nodes (fill=1) */
TEST_F(QuicklistTrackingTest, PushCreatesNewNodes) {
    quicklist *ql = quicklistNew(1, 0);

    for (int i = 0; i < 20; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "single_node_element_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_EQ(ql->len, 20ul);
    ASSERT_TRACKED_CORRECT(ql);
    quicklistRelease(ql);
}

/* 3. Pop from head and tail */
TEST_F(QuicklistTrackingTest, PopHeadAndTail) {
    quicklist *ql = quicklistNew(3, 0);

    for (int i = 0; i < 30; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "pop_test_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_CORRECT(ql);

    for (int i = 0; i < 8; i++) {
        unsigned char *data;
        size_t sz;
        long long val;
        quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
        if (data) zfree(data);
    }
    ASSERT_TRACKED_CORRECT(ql);

    for (int i = 0; i < 8; i++) {
        unsigned char *data;
        size_t sz;
        long long val;
        quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &val);
        if (data) zfree(data);
    }
    ASSERT_TRACKED_CORRECT(ql);

    quicklistRelease(ql);
}

/* 4. Delete entire node (pop all elements) */
TEST_F(QuicklistTrackingTest, DeleteEntireNode) {
    quicklist *ql = quicklistNew(2, 0);

    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "del_node_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_EQ(ql->len, 5ul);
    ASSERT_TRACKED_CORRECT(ql);

    while (quicklistCount(ql) > 0) {
        unsigned char *data;
        size_t sz;
        long long val;
        quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
        if (data) zfree(data);
        ASSERT_TRACKED_CORRECT(ql);
    }
    ASSERT_EQ(ql->tracked_data_bytes, 0ul);
    quicklistRelease(ql);
}

/* 5. Compression and decompression */
TEST_F(QuicklistTrackingTest, CompressDecompress) {
    quicklist *ql = quicklistNew(-2, 1);

    for (int i = 0; i < 500; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "compress_test_value_%d_padding_data", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_GE(ql->len, 3ul);
    bool has_compressed = false;
    for (quicklistNode *n = ql->head; n; n = n->next) {
        if (quicklistNodeIsCompressed(n)) {
            has_compressed = true;
            break;
        }
    }
    ASSERT_TRUE(has_compressed);
    ASSERT_TRACKED_CORRECT(ql);

    for (int i = 0; i < 50; i++) {
        unsigned char *data;
        size_t sz;
        long long val;
        quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
        if (data) zfree(data);
    }
    ASSERT_TRACKED_CORRECT(ql);

    for (int i = 0; i < 50; i++) {
        unsigned char *data;
        size_t sz;
        long long val;
        quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &val);
        if (data) zfree(data);
    }
    ASSERT_TRACKED_CORRECT(ql);

    for (int i = 0; i < 50; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "after_compress_push_%d", i);
        quicklistPushHead(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_CORRECT(ql);

    quicklistRelease(ql);
}

/* 6. InsertBefore / InsertAfter into non-full node */
TEST_F(QuicklistTrackingTest, InsertNonFullNode) {
    quicklist *ql = quicklistNew(-2, 0);

    for (int i = 0; i < 20; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "insert_test_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_CORRECT(ql);

    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 5, &entry);
    ASSERT_NE(iter, nullptr);
    quicklistInsertAfter(iter, &entry, (void *)"INSERTED_AFTER", 14);
    quicklistReleaseIterator(iter);
    ASSERT_TRACKED_CORRECT(ql);

    iter = quicklistGetIteratorEntryAtIdx(ql, 10, &entry);
    ASSERT_NE(iter, nullptr);
    quicklistInsertBefore(iter, &entry, (void *)"INSERTED_BEFORE", 15);
    quicklistReleaseIterator(iter);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistRelease(ql);
}

/* 7. Insert that triggers node split */
TEST_F(QuicklistTrackingTest, InsertTriggersSplit) {
    quicklist *ql = quicklistNew(3, 0);

    for (int i = 0; i < 12; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "split_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_EQ(ql->len, 4ul);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 4, &entry);
    ASSERT_NE(iter, nullptr);
    quicklistInsertAfter(iter, &entry, (void *)"SPLIT_INSERT", 12);
    quicklistReleaseIterator(iter);
    ASSERT_GT(ql->len, 4ul);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistRelease(ql);
}

/* 8a. Insert into next neighbor */
TEST_F(QuicklistTrackingTest, InsertNextNeighbor) {
    quicklist *ql = quicklistNew(4, 0);

    for (int i = 0; i < 6; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "neighbor_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_EQ(ql->len, 2ul);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 3, &entry);
    ASSERT_NE(iter, nullptr);
    quicklistInsertAfter(iter, &entry, (void *)"NEIGHBOR_NEXT", 13);
    quicklistReleaseIterator(iter);
    ASSERT_EQ(ql->len, 2ul);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistRelease(ql);
}

/* 8b. Insert into prev neighbor */
TEST_F(QuicklistTrackingTest, InsertPrevNeighbor) {
    quicklist *ql = quicklistNew(4, 0);

    for (int i = 0; i < 8; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "avprev_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_EQ(ql->len, 2ul);
    unsigned char *data;
    size_t sz;
    long long val;
    quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
    if (data) zfree(data);
    ASSERT_EQ(ql->head->count, 3u);
    ASSERT_EQ(ql->tail->count, 4u);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 3, &entry);
    ASSERT_NE(iter, nullptr);
    quicklistInsertBefore(iter, &entry, (void *)"NEIGHBOR_PREV", 13);
    quicklistReleaseIterator(iter);
    ASSERT_EQ(ql->len, 2ul);
    ASSERT_EQ(ql->head->count, 4u);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistRelease(ql);
}

/* 9. ReplaceEntry - listpack path */
TEST_F(QuicklistTrackingTest, ReplaceEntryListpack) {
    quicklist *ql = quicklistNew(-2, 0);

    for (int i = 0; i < 20; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "replace_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_CORRECT(ql);

    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 5, &entry);
    ASSERT_NE(iter, nullptr);
    quicklistReplaceEntry(iter, &entry, (void *)"S", 1);
    quicklistReleaseIterator(iter);
    ASSERT_TRACKED_CORRECT(ql);

    iter = quicklistGetIteratorEntryAtIdx(ql, 10, &entry);
    ASSERT_NE(iter, nullptr);
    char big_replace[200];
    memset(big_replace, 'X', sizeof(big_replace));
    quicklistReplaceEntry(iter, &entry, big_replace, sizeof(big_replace));
    quicklistReleaseIterator(iter);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistRelease(ql);
}

/* 10. ReplaceEntry - plain node path */
TEST_F(QuicklistTrackingTest, ReplaceEntryPlainNode) {
    quicklistSetPackedThreshold(64);

    quicklist *ql = quicklistNew(-2, 0);

    char large[128];
    memset(large, 'A', sizeof(large));
    quicklistPushTail(ql, large, sizeof(large));
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "mixed_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_CORRECT(ql);

    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
    ASSERT_NE(iter, nullptr);
    ASSERT_TRUE(QL_NODE_IS_PLAIN(entry.node));
    char new_large[256];
    memset(new_large, 'B', sizeof(new_large));
    quicklistReplaceEntry(iter, &entry, new_large, sizeof(new_large));
    quicklistReleaseIterator(iter);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistSetPackedThreshold(0);
    quicklistRelease(ql);
}

/* 11. DelRange - partial and full node deletion */
TEST_F(QuicklistTrackingTest, DelRange) {
    quicklist *ql = quicklistNew(5, 0);

    for (int i = 0; i < 30; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "delrange_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_CORRECT(ql);

    quicklistDelRange(ql, 3, 2);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistDelRange(ql, 2, 15);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistDelRange(ql, 0, 3);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistDelRange(ql, -3, 3);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistRelease(ql);
}

/* 12. Rotate */
TEST_F(QuicklistTrackingTest, Rotate) {
    quicklist *ql = quicklistNew(3, 0);

    for (int i = 0; i < 15; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "rotate_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_CORRECT(ql);

    for (int i = 0; i < 10; i++) {
        quicklistRotate(ql);
        ASSERT_TRACKED_CORRECT(ql);
    }

    quicklistRelease(ql);
}

/* 13. Dup */
TEST_F(QuicklistTrackingTest, Dup) {
    quicklist *ql = quicklistNew(-2, 0);

    for (int i = 0; i < 100; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "dup_test_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_CORRECT(ql);

    quicklist *copy = quicklistDup(ql);
    ASSERT_TRACKED_CORRECT(copy);

    for (int i = 0; i < 20; i++) {
        unsigned char *data;
        size_t sz;
        long long val;
        quicklistPop(copy, QUICKLIST_TAIL, &data, &sz, &val);
        if (data) zfree(data);
    }
    ASSERT_TRACKED_CORRECT(copy);

    quicklistRelease(copy);
    quicklistRelease(ql);
}

/* 14. Dup with compression */
TEST_F(QuicklistTrackingTest, DupWithCompression) {
    quicklist *ql = quicklistNew(-2, 1);

    for (int i = 0; i < 500; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "dup_compress_%d_padding_data_here", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_CORRECT(ql);

    quicklist *copy = quicklistDup(ql);
    ASSERT_TRACKED_CORRECT(copy);

    quicklistRelease(copy);
    quicklistRelease(ql);
}

/* 15. Node merge */
TEST_F(QuicklistTrackingTest, NodeMerge) {
    quicklist *ql = quicklistNew(4, 0);

    for (int i = 0; i < 12; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "merge_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_EQ(ql->len, 3ul);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistDelRange(ql, 4, 3);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistDelRange(ql, 5, 3);
    ASSERT_TRACKED_CORRECT(ql);

    unsigned long nodes_before = ql->len;
    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 2, &entry);
    ASSERT_NE(iter, nullptr);
    quicklistInsertAfter(iter, &entry, (void *)"MERGE_TRIGGER", 13);
    quicklistReleaseIterator(iter);
    ASSERT_LE(ql->len, nodes_before + 1);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistRelease(ql);
}

/* 16. Plain node insert */
TEST_F(QuicklistTrackingTest, PlainNodeInsert) {
    quicklistSetPackedThreshold(64);

    quicklist *ql = quicklistNew(-2, 0);

    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "small_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_CORRECT(ql);

    for (int i = 0; i < 5; i++) {
        char large[128];
        memset(large, 'P' + i, sizeof(large));
        quicklistPushTail(ql, large, sizeof(large));
        ASSERT_TRACKED_CORRECT(ql);
    }

    for (int i = 0; i < 3; i++) {
        unsigned char *data;
        size_t sz;
        long long val;
        quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &val);
        if (data) zfree(data);
        ASSERT_TRACKED_CORRECT(ql);
    }

    quicklistSetPackedThreshold(0);
    quicklistRelease(ql);
}

/* 17. Insert into empty list */
TEST_F(QuicklistTrackingTest, InsertEmptyList) {
    quicklist *ql = quicklistNew(-2, 0);

    quicklistEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.quicklist = ql;
    quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
    quicklistInsertAfter(iter, &entry, (void *)"first_ever", 10);
    quicklistReleaseIterator(iter);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistRelease(ql);
}

/* 18. Compression with interleaved operations */
TEST_F(QuicklistTrackingTest, InterleavedCompressOps) {
    quicklist *ql = quicklistNew(-2, 2);

    for (int i = 0; i < 1000; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "interleaved_%d_extra_padding_here", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_CORRECT(ql);

    for (int i = 0; i < 100; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "interleaved_new_%d", i);
        if (i % 2 == 0)
            quicklistPushHead(ql, buf, strlen(buf));
        else
            quicklistPushTail(ql, buf, strlen(buf));

        unsigned char *data;
        size_t sz;
        long long val;
        if (i % 3 == 0)
            quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
        else
            quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &val);
        if (data) zfree(data);
    }
    ASSERT_TRACKED_CORRECT(ql);

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
    ASSERT_TRACKED_CORRECT(ql);

    quicklistRelease(ql);
}

/* 19. AppendListpack / AppendPlainNode (RDB load paths) */
TEST_F(QuicklistTrackingTest, AppendListpackAndPlainNode) {
    quicklist *ql = quicklistNew(-2, 0);

    unsigned char *lp = lpNew(0);
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "rdb_lp_%d", i);
        lp = lpAppend(lp, (unsigned char *)buf, strlen(buf));
    }
    quicklistAppendListpack(ql, lp);
    ASSERT_TRACKED_CORRECT(ql);

    unsigned char *lp2 = lpNew(0);
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "rdb_lp2_%d", i);
        lp2 = lpAppend(lp2, (unsigned char *)buf, strlen(buf));
    }
    quicklistAppendListpack(ql, lp2);
    ASSERT_TRACKED_CORRECT(ql);

    size_t plain_sz = 256;
    unsigned char *plain_data = (unsigned char *)zmalloc(plain_sz);
    memset(plain_data, 'Z', plain_sz);
    quicklistAppendPlainNode(ql, plain_data, plain_sz);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistRelease(ql);
}

/* 20. DelEntry via iterator */
TEST_F(QuicklistTrackingTest, DelEntryViaIterator) {
    quicklist *ql = quicklistNew(5, 0);

    for (int i = 0; i < 25; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "delentry_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_CORRECT(ql);

    quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
    quicklistEntry entry;
    int count = 0;
    while (quicklistNext(iter, &entry)) {
        if (count % 2 == 0) {
            quicklistDelEntry(iter, &entry);
        }
        count++;
    }
    quicklistReleaseIterator(iter);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistRelease(ql);
}

/* 21. ReplaceAtIndex */
TEST_F(QuicklistTrackingTest, ReplaceAtIndex) {
    quicklist *ql = quicklistNew(-2, 0);

    for (int i = 0; i < 30; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "replaceatidx_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_CORRECT(ql);

    quicklistReplaceAtIndex(ql, 0, (void *)"HEAD_REPLACED", 13);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistReplaceAtIndex(ql, 15, (void *)"MID_REPLACED", 12);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistReplaceAtIndex(ql, 29, (void *)"TAIL_REPLACED", 13);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistRelease(ql);
}

/* 22. Same-size replacement */
TEST_F(QuicklistTrackingTest, SameSizeReplace) {
    quicklist *ql = quicklistNew(4, 0);

    for (int i = 0; i < 8; i++) {
        quicklistPushTail(ql, (void *)"aaaa", 4);
    }
    ASSERT_EQ(ql->len, 2ul);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistReplaceAtIndex(ql, 0, (void *)"bbbbbbbbbbbbbbbb", 16);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistReplaceAtIndex(ql, 7, (void *)"cccc", 4);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistReplaceAtIndex(ql, 5, (void *)"dddd", 4);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistReplaceAtIndex(ql, 6, (void *)"eeee", 4);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistRelease(ql);
}

/* 23. Empty after all pops */
TEST_F(QuicklistTrackingTest, EmptyAfterPops) {
    quicklist *ql = quicklistNew(-2, 0);

    for (int i = 0; i < 5; i++) {
        quicklistPushTail(ql, (void *)"hello", 5);
    }
    ASSERT_TRACKED_CORRECT(ql);

    while (quicklistCount(ql) > 0) {
        unsigned char *data;
        size_t sz;
        long long val;
        quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
        if (data) zfree(data);
    }
    ASSERT_EQ(ql->tracked_data_bytes, 0ul);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistRelease(ql);
}

/* 24. Replace plain node with small value (plain→packed conversion) */
TEST_F(QuicklistTrackingTest, ReplacePlainWithSmall) {
    quicklistSetPackedThreshold(64);

    quicklist *ql = quicklistNew(-2, 0);

    char large[128];
    memset(large, 'A', sizeof(large));
    quicklistPushTail(ql, large, sizeof(large));
    quicklistPushTail(ql, (void *)"small", 5);
    ASSERT_TRACKED_CORRECT(ql);

    /* Replace plain node with small value — triggers quicklistInsertAfter + __quicklistDelNode */
    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
    ASSERT_NE(iter, nullptr);
    ASSERT_TRUE(QL_NODE_IS_PLAIN(entry.node));
    quicklistReplaceEntry(iter, &entry, (void *)"tiny", 4);
    quicklistReleaseIterator(iter);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistSetPackedThreshold(0);
    quicklistRelease(ql);
}

/* 25. Large element insert with split (_quicklistInsert large element + split path) */
TEST_F(QuicklistTrackingTest, LargeElementInsertWithSplit) {
    quicklistSetPackedThreshold(64);

    quicklist *ql = quicklistNew(-2, 0);

    for (int i = 0; i < 20; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "item_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_CORRECT(ql);

    /* Insert large element in the middle — triggers split + plain node insert */
    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 10, &entry);
    ASSERT_NE(iter, nullptr);
    char large[128];
    memset(large, 'L', sizeof(large));
    quicklistInsertAfter(iter, &entry, large, sizeof(large));
    quicklistReleaseIterator(iter);
    ASSERT_TRACKED_CORRECT(ql);

    quicklistSetPackedThreshold(0);
    quicklistRelease(ql);
}

/* 26. Deep compress depth */
TEST_F(QuicklistTrackingTest, DeepCompressDepth) {
    quicklist *ql = quicklistNew(-2, 3);

    for (int i = 0; i < 2000; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "deep_compress_%d_padding_data_xxxx", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_CORRECT(ql);

    /* Pop from both ends to trigger recompression boundary changes */
    for (int i = 0; i < 200; i++) {
        unsigned char *data;
        size_t sz;
        long long val;
        if (i % 2 == 0)
            quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
        else
            quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &val);
        if (data) zfree(data);
    }
    ASSERT_TRACKED_CORRECT(ql);

    quicklistRelease(ql);
}

/* 27. Fuzzer — random operations with correctness check */
TEST_F(QuicklistTrackingTest, Fuzzer) {
    unsigned int seed = 42;
    quicklist *ql = quicklistNew(-2, 1);

    /* Seed with some data */
    for (int i = 0; i < 100; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "seed_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_CORRECT(ql);

    /* Weighted ops: ~60% adds, ~25% removes, ~15% replaces. */
    for (int i = 0; i < 2000; i++) {
        int op = rand_r(&seed) % 10;
        char buf[64];
        snprintf(buf, sizeof(buf), "fuzz_%d_%d", op, i);
        size_t len = strlen(buf);

        switch (op) {
        case 0: /* pushHead */
            quicklistPushHead(ql, buf, len);
            break;
        case 1: /* pushTail */
        case 3:
            quicklistPushTail(ql, buf, len);
            break;
        case 2: /* insertAfter */
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
        case 4: /* popHead */
            if (quicklistCount(ql) > 0) {
                unsigned char *data;
                size_t sz;
                long long val;
                quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
                if (data) zfree(data);
            }
            break;
        case 5: /* popTail */
            if (quicklistCount(ql) > 0) {
                unsigned char *data;
                size_t sz;
                long long val;
                quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &val);
                if (data) zfree(data);
            }
            break;
        case 6: /* delRange */
            if (quicklistCount(ql) > 3) {
                long idx = rand_r(&seed) % quicklistCount(ql);
                quicklistDelRange(ql, idx, 1);
            }
            break;
        case 7: /* replaceAtIndex */
        case 8:
            if (quicklistCount(ql) > 0) {
                long idx = rand_r(&seed) % quicklistCount(ql);
                quicklistReplaceAtIndex(ql, idx, buf, len);
            }
            break;
        case 9: /* rotate */
            if (quicklistCount(ql) > 0) {
                quicklistRotate(ql);
            }
            break;
        }

        if (i % 100 == 0) {
            ASSERT_TRACKED_CORRECT(ql);
        }
    }
    ASSERT_TRACKED_CORRECT(ql);

    quicklistRelease(ql);
}
