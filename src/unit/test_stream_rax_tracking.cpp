/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Integration tests for rax data tracking through actual stream operations.
 * Verifies that tracked_data_bytes is correct after XADD, trim, consumer
 * group creation, message delivery, and acknowledgment.
 */

#include "generated_wrappers.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include "listpack.h"
#include "rax.h"
#include "sds.h"
#include "server.h"
#include "stream.h"

int streamAppendItem(stream *s, robj **argv, int64_t numfields, streamID *added_id, streamID *use_id, int seq_given);
streamCG *streamCreateCG(stream *s, char *name, size_t namelen, streamID *id, long long entries_read);
streamConsumer *streamCreateConsumer(streamCG *cg, sds name, robj *key, int dbid, int flags);
streamNACK *streamCreateNACK(streamConsumer *consumer);
void streamFreeNACK(streamNACK *na);
void freeStream(stream *s);
int64_t streamTrimByLength(stream *s, long long maxlen, int approx);
void streamEncodeID(void *buf, streamID *id);

void streamDelConsumer(streamCG *cg, streamConsumer *consumer);
void streamFreeConsumer(streamConsumer *sc);
void streamFreeCG(streamCG *cg);

size_t streamConsumerGetSize(void *data);
size_t streamNACKGetSize(void *data);
size_t streamCGGetSize(void *data);
}

class StreamRaxTrackingTest : public ::testing::Test {};

/* ── Ground truth helpers ─────────────────────────────────────────────── */

/* Sum lpBytes for all listpacks in the stream's entry rax. */
static size_t computeStreamEntryDataBytes(stream *s) {
    size_t total = 0;
    raxIterator ri;
    raxStart(&ri, s->rax);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) {
        total += lpBytes((unsigned char *)ri.data);
    }
    raxStop(&ri);
    return total;
}

/* Helper to append a stream entry with field/value pairs. */
static streamID appendEntry(stream *s, const char *field, const char *value) {
    robj *argv[2];
    argv[0] = createStringObject(field, strlen(field));
    argv[1] = createStringObject(value, strlen(value));
    streamID id;
    streamAppendItem(s, argv, 1, &id, NULL, 0);
    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
    return id;
}

/* ── 1. XADD: stream entry rax tracks listpack data ──────────────────── */
TEST_F(StreamRaxTrackingTest, AppendEntriesTracksListpacks) {
    stream *s = streamNew();

    for (int i = 0; i < 100; i++) {
        char field[32], value[64];
        snprintf(field, sizeof(field), "field_%d", i);
        snprintf(value, sizeof(value), "value_%d_with_some_data", i);
        appendEntry(s, field, value);
        ASSERT_EQ(raxTrackedDataBytes(s->rax), computeStreamEntryDataBytes(s))
            << "Mismatch after entry " << i;
    }
    ASSERT_GT(raxTrackedDataBytes(s->rax), 0ul);

    freeStream(s);
}

/* ── 2. Trim: tracked bytes stay correct ───────────────────────────────── */
TEST_F(StreamRaxTrackingTest, TrimTracking) {
    stream *s = streamNew();

    /* Use enough entries with large values to force multiple rax nodes,
     * so that trim actually removes whole nodes. */
    server.stream_node_max_entries = 10;
    for (int i = 0; i < 100; i++) {
        char field[32], value[128];
        snprintf(field, sizeof(field), "f%d", i);
        snprintf(value, sizeof(value), "value_%d_with_extra_padding_to_fill_listpacks", i);
        appendEntry(s, field, value);
    }
    size_t before = raxTrackedDataBytes(s->rax);
    ASSERT_GT(before, 0ul);
    ASSERT_EQ(before, computeStreamEntryDataBytes(s));

    /* Trim to 5 entries — should remove most rax nodes */
    streamTrimByLength(s, 5, 0);
    size_t after = raxTrackedDataBytes(s->rax);
    ASSERT_LE(after, before);
    ASSERT_EQ(after, computeStreamEntryDataBytes(s));

    server.stream_node_max_entries = 100; /* restore default */
    freeStream(s);
}

/* ── 3. Consumer group creation: cgroups rax tracks streamCG data ─────── */
TEST_F(StreamRaxTrackingTest, ConsumerGroupTracking) {
    stream *s = streamNew();
    appendEntry(s, "key", "val");

    streamID id = {0, 0};
    streamCG *cg1 = streamCreateCG(s, (char *)"group1", 6, &id, 0);
    ASSERT_NE(cg1, nullptr);
    ASSERT_EQ(raxTrackedDataBytes(s->cgroups), sizeof(streamCG));

    streamCG *cg2 = streamCreateCG(s, (char *)"group2", 6, &id, 0);
    ASSERT_NE(cg2, nullptr);
    ASSERT_EQ(raxTrackedDataBytes(s->cgroups), 2 * sizeof(streamCG));

    freeStream(s);
}

/* ── 4. NACK tracking: pel rax tracks streamNACK data ─────────────────── */
TEST_F(StreamRaxTrackingTest, NACKTracking) {
    stream *s = streamNew();
    streamID id1 = appendEntry(s, "f1", "v1");
    streamID id2 = appendEntry(s, "f2", "v2");

    streamID zero = {0, 0};
    streamCG *cg = streamCreateCG(s, (char *)"mygroup", 7, &zero, 0);

    /* Create NACKs and insert into CG's PEL */
    streamNACK *nack1 = streamCreateNACK(NULL);
    streamNACK *nack2 = streamCreateNACK(NULL);
    unsigned char buf[sizeof(streamID)];
    streamEncodeID(buf, &id1);
    raxInsert(cg->pel, buf, sizeof(buf), nack1, NULL);
    streamEncodeID(buf, &id2);
    raxInsert(cg->pel, buf, sizeof(buf), nack2, NULL);

    ASSERT_EQ(raxTrackedDataBytes(cg->pel), 2 * sizeof(streamNACK));

    /* Remove one NACK */
    void *old = NULL;
    streamEncodeID(buf, &id1);
    raxRemove(cg->pel, buf, sizeof(buf), &old);
    streamFreeNACK((streamNACK *)old);
    ASSERT_EQ(raxTrackedDataBytes(cg->pel), sizeof(streamNACK));

    freeStream(s);
}

/* ── 5. Consumer tracking: consumers rax tracks streamConsumer data ──── */
TEST_F(StreamRaxTrackingTest, ConsumerTracking) {
    stream *s = streamNew();
    appendEntry(s, "f", "v");

    streamID zero = {0, 0};
    streamCG *cg = streamCreateCG(s, (char *)"grp", 3, &zero, 0);

    robj *key = createStringObject("mystream", 8);
    sds name1 = sdsnew("consumer_short");
    streamConsumer *c1 = streamCreateConsumer(cg, name1, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    ASSERT_NE(c1, nullptr);
    size_t expected1 = streamConsumerGetSize(c1);
    ASSERT_EQ(raxTrackedDataBytes(cg->consumers), expected1);

    sds name2 = sdsnew("consumer_with_a_much_longer_name");
    streamConsumer *c2 = streamCreateConsumer(cg, name2, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    ASSERT_NE(c2, nullptr);
    size_t expected2 = streamConsumerGetSize(c2);
    ASSERT_EQ(raxTrackedDataBytes(cg->consumers), expected1 + expected2);

    sdsfree(name1);
    sdsfree(name2);
    decrRefCount(key);
    freeStream(s);
}

/* ── 6. Full lifecycle: append, create CG, deliver, ack, trim ─────────── */
TEST_F(StreamRaxTrackingTest, FullLifecycle) {
    stream *s = streamNew();

    /* Append entries */
    streamID ids[50];
    for (int i = 0; i < 50; i++) {
        char f[16], v[32];
        snprintf(f, sizeof(f), "f%d", i);
        snprintf(v, sizeof(v), "val_%d", i);
        ids[i] = appendEntry(s, f, v);
    }
    ASSERT_EQ(raxTrackedDataBytes(s->rax), computeStreamEntryDataBytes(s));

    /* Create consumer group */
    streamID zero = {0, 0};
    streamCG *cg = streamCreateCG(s, (char *)"workers", 7, &zero, 0);
    ASSERT_EQ(raxTrackedDataBytes(s->cgroups), sizeof(streamCG));

    /* Simulate delivering messages: create NACKs in PEL */
    for (int i = 0; i < 10; i++) {
        streamNACK *nack = streamCreateNACK(NULL);
        unsigned char buf[sizeof(streamID)];
        streamEncodeID(buf, &ids[i]);
        raxInsert(cg->pel, buf, sizeof(buf), nack, NULL);
    }
    ASSERT_EQ(raxTrackedDataBytes(cg->pel), 10 * sizeof(streamNACK));

    /* Acknowledge (remove from PEL) */
    for (int i = 0; i < 5; i++) {
        unsigned char buf[sizeof(streamID)];
        streamEncodeID(buf, &ids[i]);
        void *old = NULL;
        raxRemove(cg->pel, buf, sizeof(buf), &old);
        streamFreeNACK((streamNACK *)old);
    }
    ASSERT_EQ(raxTrackedDataBytes(cg->pel), 5 * sizeof(streamNACK));

    /* Trim stream */
    size_t before_trim = raxTrackedDataBytes(s->rax);
    streamTrimByLength(s, 10, 0);
    ASSERT_LE(raxTrackedDataBytes(s->rax), before_trim);
    ASSERT_EQ(raxTrackedDataBytes(s->rax), computeStreamEntryDataBytes(s));

    freeStream(s);
}

/* ── 7. streamDelConsumer: removes NACKs from cg->pel + consumer from
 *       cg->consumers ──────────────────────────────────────────────────── */
TEST_F(StreamRaxTrackingTest, DelConsumer) {
    stream *s = streamNew();
    appendEntry(s, "f", "v");

    streamID zero = {0, 0};
    streamCG *cg = streamCreateCG(s, (char *)"grp", 3, &zero, 0);

    robj *key = createStringObject("mystream", 8);
    sds name1 = sdsnew("consumer_one");
    streamConsumer *c1 = streamCreateConsumer(cg, name1, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);

    /* Deliver NACKs to this consumer */
    streamID ids[5];
    for (int i = 0; i < 5; i++) {
        char f[16], v[16];
        snprintf(f, sizeof(f), "f%d", i);
        snprintf(v, sizeof(v), "v%d", i);
        ids[i] = appendEntry(s, f, v);

        streamNACK *nack = streamCreateNACK(c1);
        unsigned char buf[sizeof(streamID)];
        streamEncodeID(buf, &ids[i]);
        raxInsert(cg->pel, buf, sizeof(buf), nack, NULL);
        raxInsert(c1->pel, buf, sizeof(buf), nack, NULL);
    }

    ASSERT_EQ(raxTrackedDataBytes(cg->pel), 5 * sizeof(streamNACK));
    size_t consumer_tracked = raxTrackedDataBytes(cg->consumers);
    ASSERT_GT(consumer_tracked, 0ul);

    /* Delete the consumer — should remove all NACKs from cg->pel
     * and the consumer from cg->consumers */
    streamDelConsumer(cg, c1);

    ASSERT_EQ(raxTrackedDataBytes(cg->pel), 0ul);
    ASSERT_EQ(raxTrackedDataBytes(cg->consumers), 0ul);

    sdsfree(name1);
    decrRefCount(key);
    freeStream(s);
}

/* ── 8. XACK path: remove NACK from group->pel ───────────────────────── */
TEST_F(StreamRaxTrackingTest, AckRemovesNACK) {
    stream *s = streamNew();

    streamID zero = {0, 0};
    streamCG *cg = streamCreateCG(s, (char *)"grp", 3, &zero, 0);

    robj *key = createStringObject("mystream", 8);
    sds cname = sdsnew("worker");
    streamConsumer *consumer = streamCreateConsumer(cg, cname, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);

    /* Create 10 NACKs */
    streamID ids[10];
    for (int i = 0; i < 10; i++) {
        char f[16], v[16];
        snprintf(f, sizeof(f), "f%d", i);
        snprintf(v, sizeof(v), "v%d", i);
        ids[i] = appendEntry(s, f, v);

        streamNACK *nack = streamCreateNACK(consumer);
        unsigned char buf[sizeof(streamID)];
        streamEncodeID(buf, &ids[i]);
        raxInsert(cg->pel, buf, sizeof(buf), nack, NULL);
        raxInsert(consumer->pel, buf, sizeof(buf), nack, NULL);
    }
    ASSERT_EQ(raxTrackedDataBytes(cg->pel), 10 * sizeof(streamNACK));

    /* Simulate XACK: remove NACKs from both PELs and free */
    for (int i = 0; i < 7; i++) {
        unsigned char buf[sizeof(streamID)];
        streamEncodeID(buf, &ids[i]);
        void *result;
        raxFind(cg->pel, buf, sizeof(buf), &result);
        streamNACK *nack = (streamNACK *)result;

        raxRemove(cg->pel, buf, sizeof(buf), NULL);
        raxRemove(consumer->pel, buf, sizeof(buf), NULL);
        streamFreeNACK(nack);
    }
    ASSERT_EQ(raxTrackedDataBytes(cg->pel), 3 * sizeof(streamNACK));

    sdsfree(cname);
    decrRefCount(key);
    freeStream(s);
}

/* ── 9. XGROUP DESTROY: remove CG from s->cgroups ────────────────────── */
TEST_F(StreamRaxTrackingTest, DestroyConsumerGroup) {
    stream *s = streamNew();
    appendEntry(s, "f", "v");

    streamID zero = {0, 0};
    streamCreateCG(s, (char *)"grp1", 4, &zero, 0);
    streamCreateCG(s, (char *)"grp2", 4, &zero, 0);
    streamCreateCG(s, (char *)"grp3", 4, &zero, 0);
    ASSERT_EQ(raxTrackedDataBytes(s->cgroups), 3 * sizeof(streamCG));

    /* Simulate XGROUP DESTROY grp2 */
    void *result;
    raxFind(s->cgroups, (unsigned char *)"grp2", 4, &result);
    streamCG *cg = (streamCG *)result;
    raxRemove(s->cgroups, (unsigned char *)"grp2", 4, NULL);
    streamFreeCG(cg);

    ASSERT_EQ(raxTrackedDataBytes(s->cgroups), 2 * sizeof(streamCG));

    freeStream(s);
}

/* ── 10. streamIteratorRemoveEntry: in-place lpReplaceInteger delta ──── */
TEST_F(StreamRaxTrackingTest, IteratorRemoveEntry) {
    stream *s = streamNew();

    /* Insert enough entries to create content */
    streamID ids[20];
    for (int i = 0; i < 20; i++) {
        char f[16], v[32];
        snprintf(f, sizeof(f), "field%d", i);
        snprintf(v, sizeof(v), "value_%d_data", i);
        ids[i] = appendEntry(s, f, v);
    }
    ASSERT_EQ(raxTrackedDataBytes(s->rax), computeStreamEntryDataBytes(s));

    /* Remove entries via iterator (simulates XDEL) */
    for (int i = 0; i < 10; i++) {
        streamIterator si;
        streamIteratorStart(&si, s, &ids[i], &ids[i], 0);
        streamID myid;
        int64_t numfields;
        if (streamIteratorGetID(&si, &myid, &numfields)) {
            streamIteratorRemoveEntry(&si, &myid);
        }
        streamIteratorStop(&si);
        ASSERT_EQ(raxTrackedDataBytes(s->rax), computeStreamEntryDataBytes(s))
            << "Mismatch after removing entry " << i;
    }

    freeStream(s);
}
