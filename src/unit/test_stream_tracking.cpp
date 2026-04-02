/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Unit tests for stream tracked_data_bytes and tracked_overhead.
 */

#include "generated_wrappers.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

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
void streamDelConsumer(streamCG *cg, streamConsumer *consumer);
void streamFreeCG(streamCG *cg);
void freeStream(stream *s);
int64_t streamTrimByLength(stream *s, long long maxlen, int approx);
int64_t streamTrimByID(stream *s, streamID minid, int approx);
void streamEncodeID(void *buf, streamID *id);
robj *streamDup(robj *o);
size_t raxComputeLogicalSize(rax *rax);
}

class StreamTrackingTest : public ::testing::Test {};

/* ── Ground truth ────────────────────────────────────────────────────── */

static size_t computeDataBytesWalk(stream *s) {
    size_t total = 0;
    raxIterator ri;
    raxStart(&ri, s->rax);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) total += lpBytes((unsigned char *)ri.data);
    raxStop(&ri);
    if (s->cgroups) {
        raxStart(&ri, s->cgroups);
        raxSeek(&ri, "^", NULL, 0);
        while (raxNext(&ri)) {
            streamCG *cg = (streamCG *)ri.data;
            total += sizeof(streamCG);
            total += raxSize(cg->pel) * sizeof(streamNACK);
            raxIterator ci;
            raxStart(&ci, cg->consumers);
            raxSeek(&ci, "^", NULL, 0);
            while (raxNext(&ci)) {
                streamConsumer *sc = (streamConsumer *)ci.data;
                total += sizeof(streamConsumer) + sdsReqSize(sdslen(sc->name), sdsType(sc->name));
            }
            raxStop(&ci);
        }
        raxStop(&ri);
    }
    return total;
}

static size_t computeOverheadWalk(stream *s) {
    size_t total = 0;
    total += raxComputeLogicalSize(s->rax);
    if (s->cgroups) {
        total += raxComputeLogicalSize(s->cgroups);
        raxIterator ri;
        raxStart(&ri, s->cgroups);
        raxSeek(&ri, "^", NULL, 0);
        while (raxNext(&ri)) {
            streamCG *cg = (streamCG *)ri.data;
            total += raxComputeLogicalSize(cg->pel);
            total += raxComputeLogicalSize(cg->consumers);
            raxIterator ci;
            raxStart(&ci, cg->consumers);
            raxSeek(&ci, "^", NULL, 0);
            while (raxNext(&ci)) {
                streamConsumer *sc = (streamConsumer *)ci.data;
                total += raxComputeLogicalSize(sc->pel);
            }
            raxStop(&ci);
        }
        raxStop(&ri);
    }
    return total;
}

#define ASSERT_STREAM_TRACKING(s)                                           \
    do {                                                                    \
        ASSERT_EQ((s)->tracked_data_bytes, computeDataBytesWalk(s))         \
            << "tracked_data_bytes mismatch";                               \
        ASSERT_EQ((s)->tracked_overhead, computeOverheadWalk(s))            \
            << "tracked_overhead mismatch";                                 \
    } while (0)

/* ── Helpers ─────────────────────────────────────────────────────────── */

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

/* ── Tests ───────────────────────────────────────────────────────────── */

TEST_F(StreamTrackingTest, AppendEntries) {
    stream *s = streamNew();
    server.stream_node_max_entries = 10;
    for (int i = 0; i < 100; i++) {
        char f[16], v[32];
        snprintf(f, sizeof(f), "f%d", i);
        snprintf(v, sizeof(v), "value_%d_data", i);
        appendEntry(s, f, v);
        ASSERT_STREAM_TRACKING(s);
    }
    ASSERT_EQ(raxSize(s->rax), 10ul);
    server.stream_node_max_entries = 100;
    freeStream(s);
}

TEST_F(StreamTrackingTest, Trim) {
    stream *s = streamNew();
    server.stream_node_max_entries = 10;
    for (int i = 0; i < 100; i++) {
        char f[16], v[64];
        snprintf(f, sizeof(f), "f%d", i);
        snprintf(v, sizeof(v), "value_%d_with_padding", i);
        appendEntry(s, f, v);
    }
    ASSERT_STREAM_TRACKING(s);
    streamTrimByLength(s, 5, 0);
    ASSERT_STREAM_TRACKING(s);
    server.stream_node_max_entries = 100;
    freeStream(s);
}

TEST_F(StreamTrackingTest, TrimByID) {
    stream *s = streamNew();
    server.stream_node_max_entries = 10;
    streamID ids[50];
    for (int i = 0; i < 50; i++) {
        char f[16], v[32];
        snprintf(f, sizeof(f), "f%d", i);
        snprintf(v, sizeof(v), "value_%d", i);
        ids[i] = appendEntry(s, f, v);
    }
    ASSERT_STREAM_TRACKING(s);
    ASSERT_EQ(raxSize(s->rax), 5ul);
    streamTrimByID(s, ids[35], 0);
    ASSERT_STREAM_TRACKING(s);
    ASSERT_LT(raxSize(s->rax), 5ul);
    server.stream_node_max_entries = 100;
    freeStream(s);
}

TEST_F(StreamTrackingTest, TrimEncodingBoundary) {
    stream *s = streamNew();
    server.stream_node_max_entries = 200;
    server.stream_node_max_bytes = 0;
    for (int i = 0; i < 129; i++) {
        char f[16], v[16];
        snprintf(f, sizeof(f), "f%d", i);
        snprintf(v, sizeof(v), "v%d", i);
        appendEntry(s, f, v);
    }
    ASSERT_STREAM_TRACKING(s);
    ASSERT_EQ(raxSize(s->rax), 1ul);
    size_t bytes_before = s->tracked_data_bytes;
    streamTrimByLength(s, 127, 0);
    ASSERT_STREAM_TRACKING(s);
    ASSERT_EQ(bytes_before - s->tracked_data_bytes, 1ul);
    server.stream_node_max_entries = 100;
    server.stream_node_max_bytes = 4096;
    freeStream(s);
}

TEST_F(StreamTrackingTest, IteratorRemoveAll) {
    stream *s = streamNew();
    streamID ids[5];
    for (int i = 0; i < 5; i++) {
        char f[16], v[16];
        snprintf(f, sizeof(f), "f%d", i);
        snprintf(v, sizeof(v), "v%d", i);
        ids[i] = appendEntry(s, f, v);
    }
    for (int i = 0; i < 5; i++) {
        streamIterator si;
        streamIteratorStart(&si, s, &ids[i], &ids[i], 0);
        streamID myid;
        int64_t numfields;
        if (streamIteratorGetID(&si, &myid, &numfields)) {
            streamIteratorRemoveEntry(&si, &myid);
        }
        streamIteratorStop(&si);
        ASSERT_STREAM_TRACKING(s);
    }
    ASSERT_EQ(s->tracked_data_bytes, 0ul);
    freeStream(s);
}

TEST_F(StreamTrackingTest, ConsumerGroupCreate) {
    stream *s = streamNew();
    appendEntry(s, "f", "v");
    streamID zero = {0, 0};
    streamCreateCG(s, (char *)"grp1", 4, &zero, 0);
    streamCreateCG(s, (char *)"grp2", 4, &zero, 0);
    ASSERT_STREAM_TRACKING(s);
    freeStream(s);
}

TEST_F(StreamTrackingTest, FullLifecycle) {
    stream *s = streamNew();
    ASSERT_STREAM_TRACKING(s);
    streamID ids[30];
    for (int i = 0; i < 30; i++) {
        char f[16], v[32];
        snprintf(f, sizeof(f), "f%d", i);
        snprintf(v, sizeof(v), "val_%d", i);
        ids[i] = appendEntry(s, f, v);
    }
    ASSERT_STREAM_TRACKING(s);
    streamID zero = {0, 0};
    streamCG *cg = streamCreateCG(s, (char *)"workers", 7, &zero, 0);
    ASSERT_STREAM_TRACKING(s);
    robj *key = createStringObject("mystream", 8);
    sds name1 = sdsnew("alice");
    sds name2 = sdsnew("bob_with_longer_name");
    streamConsumer *c1 = streamCreateConsumer(cg, name1, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    if (c1) raxSetExternalLogicalSize(c1->pel, &s->tracked_overhead);
    streamConsumer *c2 = streamCreateConsumer(cg, name2, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    if (c2) raxSetExternalLogicalSize(c2->pel, &s->tracked_overhead);
    ASSERT_STREAM_TRACKING(s);
    for (int i = 0; i < 10; i++) {
        streamNACK *nack = streamCreateNACK(c1);
        unsigned char buf[sizeof(streamID)];
        streamEncodeID(buf, &ids[i]);
        raxInsert(cg->pel, buf, sizeof(buf), nack, NULL);
        raxInsert(c1->pel, buf, sizeof(buf), nack, NULL);
    }
    ASSERT_STREAM_TRACKING(s);
    for (int i = 0; i < 5; i++) {
        unsigned char buf[sizeof(streamID)];
        streamEncodeID(buf, &ids[i]);
        void *result;
        raxFind(cg->pel, buf, sizeof(buf), &result);
        raxRemove(cg->pel, buf, sizeof(buf), NULL);
        raxRemove(c1->pel, buf, sizeof(buf), NULL);
        streamFreeNACK((streamNACK *)result);
    }
    ASSERT_STREAM_TRACKING(s);
    streamDelConsumer(cg, c2);
    ASSERT_STREAM_TRACKING(s);
    streamTrimByLength(s, 10, 0);
    ASSERT_STREAM_TRACKING(s);
    sdsfree(name1);
    sdsfree(name2);
    decrRefCount(key);
    freeStream(s);
}

TEST_F(StreamTrackingTest, DestroyConsumerGroup) {
    stream *s = streamNew();
    appendEntry(s, "f", "v");
    streamID zero = {0, 0};
    streamCreateCG(s, (char *)"grp1", 4, &zero, 0);
    streamCG *cg2 = streamCreateCG(s, (char *)"grp2", 4, &zero, 0);
    robj *key = createStringObject("mystream", 8);
    sds name1 = sdsnew("worker_alpha");
    sds name2 = sdsnew("worker_beta_longer");
    streamConsumer *c1 = streamCreateConsumer(cg2, name1, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    raxSetExternalLogicalSize(c1->pel, &s->tracked_overhead);
    streamConsumer *c2 = streamCreateConsumer(cg2, name2, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    raxSetExternalLogicalSize(c2->pel, &s->tracked_overhead);
    streamID ids[10];
    for (int i = 0; i < 10; i++) {
        char f[16], v[16];
        snprintf(f, sizeof(f), "f%d", i);
        snprintf(v, sizeof(v), "v%d", i);
        ids[i] = appendEntry(s, f, v);
    }
    for (int i = 0; i < 7; i++) {
        streamNACK *nack = streamCreateNACK(i < 4 ? c1 : c2);
        unsigned char buf[sizeof(streamID)];
        streamEncodeID(buf, &ids[i]);
        streamConsumer *target = (i < 4 ? c1 : c2);
        raxInsert(cg2->pel, buf, sizeof(buf), nack, NULL);
        raxInsert(target->pel, buf, sizeof(buf), nack, NULL);
    }
    ASSERT_STREAM_TRACKING(s);
    raxRemove(s->cgroups, (unsigned char *)"grp2", 4, NULL);
    streamFreeCG(cg2);
    ASSERT_STREAM_TRACKING(s);
    ASSERT_GT(s->tracked_overhead, 0ul);
    sdsfree(name1);
    sdsfree(name2);
    decrRefCount(key);
    freeStream(s);
}

TEST_F(StreamTrackingTest, DelConsumerWithNACKs) {
    stream *s = streamNew();
    appendEntry(s, "f", "v");
    streamID zero = {0, 0};
    streamCG *cg = streamCreateCG(s, (char *)"grp", 3, &zero, 0);
    robj *key = createStringObject("mystream", 8);
    sds name = sdsnew("busy_consumer");
    streamConsumer *consumer = streamCreateConsumer(cg, name, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    raxSetExternalLogicalSize(consumer->pel, &s->tracked_overhead);
    streamID ids[8];
    for (int i = 0; i < 8; i++) {
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
    ASSERT_STREAM_TRACKING(s);
    streamDelConsumer(cg, consumer);
    ASSERT_STREAM_TRACKING(s);
    sdsfree(name);
    decrRefCount(key);
    freeStream(s);
}

TEST_F(StreamTrackingTest, StreamDup) {
    stream *s = streamNew();
    for (int i = 0; i < 20; i++) {
        char f[16], v[32];
        snprintf(f, sizeof(f), "f%d", i);
        snprintf(v, sizeof(v), "value_%d", i);
        appendEntry(s, f, v);
    }
    streamID zero = {0, 0};
    streamCG *cg = streamCreateCG(s, (char *)"grp", 3, &zero, 0);
    robj *key = createStringObject("mystream", 8);
    sds name = sdsnew("consumer_one");
    streamConsumer *consumer = streamCreateConsumer(cg, name, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    raxSetExternalLogicalSize(consumer->pel, &s->tracked_overhead);
    streamID ids[5];
    for (int i = 0; i < 5; i++) {
        char f[16], v[16];
        snprintf(f, sizeof(f), "nf%d", i);
        snprintf(v, sizeof(v), "nv%d", i);
        ids[i] = appendEntry(s, f, v);
        streamNACK *nack = streamCreateNACK(consumer);
        unsigned char buf[sizeof(streamID)];
        streamEncodeID(buf, &ids[i]);
        raxInsert(cg->pel, buf, sizeof(buf), nack, NULL);
        raxInsert(consumer->pel, buf, sizeof(buf), nack, NULL);
    }
    ASSERT_STREAM_TRACKING(s);
    robj *orig = createStreamObject();
    freeStream(static_cast<stream *>(objectGetVal(orig)));
    objectSetVal(orig, s);
    robj *copy = streamDup(orig);
    stream *new_s = (stream *)objectGetVal(copy);
    ASSERT_STREAM_TRACKING(new_s);
    ASSERT_EQ(s->tracked_data_bytes, new_s->tracked_data_bytes);
    ASSERT_EQ(s->tracked_overhead, new_s->tracked_overhead);
    objectSetVal(orig, NULL);
    decrRefCount(orig);
    decrRefCount(copy);
    sdsfree(name);
    decrRefCount(key);
    freeStream(s);
}

TEST_F(StreamTrackingTest, Fuzzer) {
    unsigned seed = static_cast<unsigned>(time(nullptr)) ^ static_cast<unsigned>(getpid());
    srand(seed);
    printf("  Fuzzer seed: %u\n", seed);
    stream *s = streamNew();
    robj *key = createStringObject("mystream", 8);
    streamID zero = {0, 0};
    streamCG *cgs[3];
    for (int i = 0; i < 3; i++) {
        char cgname[16];
        snprintf(cgname, sizeof(cgname), "cg%d", i);
        cgs[i] = streamCreateCG(s, cgname, strlen(cgname), &zero, 0);
    }
    ASSERT_STREAM_TRACKING(s);
    std::vector<streamID> ids;
    std::vector<std::pair<int, streamID>> pending;
    const int NUM_OPS = 2000;
    for (int op = 0; op < NUM_OPS; op++) {
        int action = rand() % 100;
        if (action < 40 || ids.empty()) {
            if (ids.size() < 2048) {
                char f[32];
                snprintf(f, sizeof(f), "field_%d", op);
                size_t vlen = 5 + (static_cast<size_t>(rand()) % 50);
                std::string val(vlen, 'a' + (rand() % 26));
                streamID id = appendEntry(s, f, val.c_str());
                ids.push_back(id);
            }
        } else if (action < 55 && ids.size() > 20) {
            streamTrimByLength(s, ids.size() / 2, 0);
        } else if (action < 70) {
            int cg_idx = rand() % 3;
            char cname[32];
            snprintf(cname, sizeof(cname), "consumer_%d_%d", op, rand() % 1000);
            sds sname = sdsnew(cname);
            streamConsumer *c = streamCreateConsumer(cgs[cg_idx], sname, key, 0,
                                                     SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
            if (c) raxSetExternalLogicalSize(c->pel, &s->tracked_overhead);
            sdsfree(sname);
        } else if (action < 85 && !ids.empty()) {
            int cg_idx = rand() % 3;
            if (raxSize(cgs[cg_idx]->consumers) > 0) {
                raxIterator ci;
                raxStart(&ci, cgs[cg_idx]->consumers);
                raxSeek(&ci, "^", NULL, 0);
                raxNext(&ci);
                streamConsumer *c = (streamConsumer *)ci.data;
                raxStop(&ci);
                int idx = rand() % ids.size();
                unsigned char buf[sizeof(streamID)];
                streamEncodeID(buf, &ids[idx]);
                streamNACK *nack = streamCreateNACK(c);
                if (raxTryInsert(cgs[cg_idx]->pel, buf, sizeof(buf), nack, NULL)) {
                    raxInsert(c->pel, buf, sizeof(buf), nack, NULL);
                    pending.push_back({cg_idx, ids[idx]});
                } else {
                    streamFreeNACK(nack);
                }
            }
        } else if (!pending.empty()) {
            int idx = rand() % pending.size();
            auto [cg_idx, ack_id] = pending[idx];
            unsigned char buf[sizeof(streamID)];
            streamEncodeID(buf, &ack_id);
            void *result;
            if (raxFind(cgs[cg_idx]->pel, buf, sizeof(buf), &result)) {
                streamNACK *nack = (streamNACK *)result;
                streamConsumer *nack_consumer = nack->consumer;
                raxRemove(cgs[cg_idx]->pel, buf, sizeof(buf), NULL);
                raxRemove(nack_consumer->pel, buf, sizeof(buf), NULL);
                streamFreeNACK(nack);
            }
            pending.erase(pending.begin() + idx);
        }
        if (op % 50 == 0) ASSERT_STREAM_TRACKING(s);
    }
    ASSERT_STREAM_TRACKING(s);
    decrRefCount(key);
    freeStream(s);
}
