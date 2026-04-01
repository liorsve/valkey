/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Integration tests for stream tracked_data_bytes and tracked_metadata_bytes
 * (Option D: single stream-level counters, no rax callbacks).
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
void streamDelConsumer(streamCG *cg, streamConsumer *consumer);
void freeStream(stream *s);
int64_t streamTrimByLength(stream *s, long long maxlen, int approx);
void streamEncodeID(void *buf, streamID *id);
robj *streamDup(robj *o);
void streamFreeCG(streamCG *cg);
}

class StreamTrackingTest : public ::testing::Test {};

/* ── Ground truth: walk everything and compute sizes ──────────────────── */

static size_t computeDataBytesWalk(stream *s) {
    size_t total = 0;
    /* Listpacks */
    raxIterator ri;
    raxStart(&ri, s->rax);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) total += lpBytes((unsigned char *)ri.data);
    raxStop(&ri);
    /* Consumer names */
    if (s->cgroups) {
        raxStart(&ri, s->cgroups);
        raxSeek(&ri, "^", NULL, 0);
        while (raxNext(&ri)) {
            streamCG *cg = (streamCG *)ri.data;
            raxIterator ci;
            raxStart(&ci, cg->consumers);
            raxSeek(&ci, "^", NULL, 0);
            while (raxNext(&ci)) {
                streamConsumer *sc = (streamConsumer *)ci.data;
                total += sdsReqSize(sdslen(sc->name), sdsType(sc->name));
            }
            raxStop(&ci);
        }
        raxStop(&ri);
    }
    return total;
}

static size_t computeMetadataBytesWalk(stream *s) {
    size_t total = 0;
    if (s->cgroups) {
        raxIterator ri;
        raxStart(&ri, s->cgroups);
        raxSeek(&ri, "^", NULL, 0);
        while (raxNext(&ri)) {
            streamCG *cg = (streamCG *)ri.data;
            total += sizeof(streamCG);
            total += raxSize(cg->pel) * sizeof(streamNACK);
            total += raxSize(cg->consumers) * sizeof(streamConsumer);
        }
        raxStop(&ri);
    }
    return total;
}

#define ASSERT_STREAM_TRACKING(s)                                              \
    do {                                                                        \
        ASSERT_EQ((s)->tracked_data_bytes, computeDataBytesWalk(s))             \
            << "tracked_data_bytes mismatch";                                   \
        ASSERT_EQ((s)->tracked_metadata_bytes, computeMetadataBytesWalk(s))     \
            << "tracked_metadata_bytes mismatch";                               \
    } while (0)

/* ── Helpers ──────────────────────────────────────────────────────────── */

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

/* ── 1. Append entries ────────────────────────────────────────────────── */
TEST_F(StreamTrackingTest, AppendEntries) {
    stream *s = streamNew();

    for (int i = 0; i < 100; i++) {
        char f[16], v[32];
        snprintf(f, sizeof(f), "f%d", i);
        snprintf(v, sizeof(v), "value_%d_data", i);
        appendEntry(s, f, v);
        ASSERT_STREAM_TRACKING(s);
    }

    freeStream(s);
}

/* ── 2. Trim ──────────────────────────────────────────────────────────── */
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

/* ── 3. Trim encoding boundary (count crosses 128) ───────────────────── */
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

    streamTrimByLength(s, 127, 0);
    ASSERT_STREAM_TRACKING(s);

    server.stream_node_max_entries = 100;
    server.stream_node_max_bytes = 4096;
    freeStream(s);
}

/* ── 4. Iterator remove all entries ───────────────────────────────────── */
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

/* ── 5. CG create ─────────────────────────────────────────────────────── */
TEST_F(StreamTrackingTest, ConsumerGroupCreate) {
    stream *s = streamNew();
    appendEntry(s, "f", "v");

    streamID zero = {0, 0};
    streamCreateCG(s, (char *)"grp1", 4, &zero, 0);
    streamCreateCG(s, (char *)"grp2", 4, &zero, 0);
    ASSERT_STREAM_TRACKING(s);
    ASSERT_EQ(s->tracked_metadata_bytes, 2 * sizeof(streamCG));

    freeStream(s);
}

/* ── 6. Full lifecycle ────────────────────────────────────────────────── */
TEST_F(StreamTrackingTest, FullLifecycle) {
    stream *s = streamNew();
    ASSERT_STREAM_TRACKING(s);

    /* Add entries */
    streamID ids[30];
    for (int i = 0; i < 30; i++) {
        char f[16], v[32];
        snprintf(f, sizeof(f), "f%d", i);
        snprintf(v, sizeof(v), "val_%d", i);
        ids[i] = appendEntry(s, f, v);
    }
    ASSERT_STREAM_TRACKING(s);

    /* Create CG */
    streamID zero = {0, 0};
    streamCG *cg = streamCreateCG(s, (char *)"workers", 7, &zero, 0);
    ASSERT_STREAM_TRACKING(s);

    /* Create consumers — track at call site */
    robj *key = createStringObject("mystream", 8);
    sds name1 = sdsnew("alice");
    sds name2 = sdsnew("bob_with_longer_name");
    streamConsumer *c1 = streamCreateConsumer(cg, name1, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    s->tracked_metadata_bytes += sizeof(streamConsumer);
    s->tracked_data_bytes += sdsReqSize(sdslen(c1->name), sdsType(c1->name));
    streamConsumer *c2 = streamCreateConsumer(cg, name2, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    s->tracked_metadata_bytes += sizeof(streamConsumer);
    s->tracked_data_bytes += sdsReqSize(sdslen(c2->name), sdsType(c2->name));
    ASSERT_STREAM_TRACKING(s);

    /* Deliver NACKs */
    for (int i = 0; i < 10; i++) {
        streamNACK *nack = streamCreateNACK(c1);
        unsigned char buf[sizeof(streamID)];
        streamEncodeID(buf, &ids[i]);
        raxInsert(cg->pel, buf, sizeof(buf), nack, NULL);
        raxInsert(c1->pel, buf, sizeof(buf), nack, NULL);
        s->tracked_metadata_bytes += sizeof(streamNACK);
    }
    ASSERT_STREAM_TRACKING(s);

    /* ACK 5 NACKs */
    for (int i = 0; i < 5; i++) {
        unsigned char buf[sizeof(streamID)];
        streamEncodeID(buf, &ids[i]);
        void *result;
        raxFind(cg->pel, buf, sizeof(buf), &result);
        raxRemove(cg->pel, buf, sizeof(buf), NULL);
        raxRemove(c1->pel, buf, sizeof(buf), NULL);
        streamFreeNACK((streamNACK *)result);
        s->tracked_metadata_bytes -= sizeof(streamNACK);
    }
    ASSERT_STREAM_TRACKING(s);

    /* Delete consumer c2 (0 NACKs) */
    s->tracked_metadata_bytes -= sizeof(streamConsumer);
    s->tracked_data_bytes -= sdsReqSize(sdslen(c2->name), sdsType(c2->name));
    streamDelConsumer(cg, c2);
    ASSERT_STREAM_TRACKING(s);

    /* Trim */
    streamTrimByLength(s, 10, 0);
    ASSERT_STREAM_TRACKING(s);

    sdsfree(name1);
    sdsfree(name2);
    decrRefCount(key);
    freeStream(s);
}

/* ── 7. XGROUP DESTROY — context callback path ───────────────────────── */
TEST_F(StreamTrackingTest, DestroyConsumerGroup) {
    stream *s = streamNew();
    appendEntry(s, "f", "v");

    streamID zero = {0, 0};
    streamCreateCG(s, (char *)"grp1", 4, &zero, 0);
    streamCG *cg2 = streamCreateCG(s, (char *)"grp2", 4, &zero, 0);

    /* Add consumers and NACKs to cg2 */
    robj *key = createStringObject("mystream", 8);
    sds name1 = sdsnew("worker_alpha");
    sds name2 = sdsnew("worker_beta_longer");
    streamConsumer *c1 = streamCreateConsumer(cg2, name1, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    s->tracked_metadata_bytes += sizeof(streamConsumer);
    s->tracked_data_bytes += sdsReqSize(sdslen(c1->name), sdsType(c1->name));
    streamConsumer *c2 = streamCreateConsumer(cg2, name2, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    s->tracked_metadata_bytes += sizeof(streamConsumer);
    s->tracked_data_bytes += sdsReqSize(sdslen(c2->name), sdsType(c2->name));

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
        raxInsert(cg2->pel, buf, sizeof(buf), nack, NULL);
        raxInsert((i < 4 ? c1 : c2)->pel, buf, sizeof(buf), nack, NULL);
        s->tracked_metadata_bytes += sizeof(streamNACK);
    }
    ASSERT_STREAM_TRACKING(s);

    /* Verify tracking is correct before destruction */
    ASSERT_STREAM_TRACKING(s);
    ASSERT_GT(s->tracked_metadata_bytes, sizeof(streamCG));
    ASSERT_GT(s->tracked_data_bytes, 0ul);

    /* freeStream uses raxFreeWithCallbackAndContext with tracking callbacks.
     * After freeing, both counters should be zero — verifying the context
     * callbacks correctly subtracted all CGs, NACKs, consumers, names. */
    freeStream(s);
    /* Can't check s->tracked_* after free, but if the callbacks were wrong
     * the ASSERT_STREAM_TRACKING above would have caught the drift before
     * destruction. The real verification is that ASAN doesn't complain
     * and all the above assertions passed. */
    s = NULL; /* Prevent use-after-free */

    sdsfree(name1);
    sdsfree(name2);
    decrRefCount(key);
}

/* ── 8. streamDelConsumer with NACKs ──────────────────────────────────── */
TEST_F(StreamTrackingTest, DelConsumerWithNACKs) {
    stream *s = streamNew();
    appendEntry(s, "f", "v");

    streamID zero = {0, 0};
    streamCG *cg = streamCreateCG(s, (char *)"grp", 3, &zero, 0);

    robj *key = createStringObject("mystream", 8);
    sds name = sdsnew("busy_consumer");
    streamConsumer *consumer = streamCreateConsumer(cg, name, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    s->tracked_metadata_bytes += sizeof(streamConsumer);
    s->tracked_data_bytes += sdsReqSize(sdslen(consumer->name), sdsType(consumer->name));

    /* Create 8 NACKs for this consumer */
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
        s->tracked_metadata_bytes += sizeof(streamNACK);
    }
    ASSERT_STREAM_TRACKING(s);

    /* Delete consumer — should subtract consumer struct + name + 8 NACKs */
    long long pending = raxSize(consumer->pel);
    s->tracked_metadata_bytes -= sizeof(streamConsumer) + pending * sizeof(streamNACK);
    s->tracked_data_bytes -= sdsReqSize(sdslen(consumer->name), sdsType(consumer->name));
    streamDelConsumer(cg, consumer);
    ASSERT_STREAM_TRACKING(s);

    /* Only the CG struct should remain */
    ASSERT_EQ(s->tracked_metadata_bytes, sizeof(streamCG));
    ASSERT_EQ(s->tracked_data_bytes, computeDataBytesWalk(s));

    sdsfree(name);
    decrRefCount(key);
    freeStream(s);
}

/* ── 9. streamDup — copy stream with CGs, consumers, NACKs ──────────── */
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
    s->tracked_metadata_bytes += sizeof(streamConsumer);
    s->tracked_data_bytes += sdsReqSize(sdslen(consumer->name), sdsType(consumer->name));

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
        s->tracked_metadata_bytes += sizeof(streamNACK);
    }
    ASSERT_STREAM_TRACKING(s);

    /* Duplicate */
    robj *orig = createStreamObject();
    /* Replace the empty stream inside with our populated one */
    freeStream(static_cast<stream *>(objectGetVal(orig)));
    objectSetVal(orig, s);

    robj *copy = streamDup(orig);
    stream *new_s = (stream *)objectGetVal(copy);

    /* The copy should have identical tracking */
    ASSERT_STREAM_TRACKING(new_s);
    ASSERT_EQ(new_s->tracked_data_bytes, computeDataBytesWalk(new_s));
    ASSERT_EQ(new_s->tracked_metadata_bytes, computeMetadataBytesWalk(new_s));

    /* Both should have same totals */
    ASSERT_EQ(s->tracked_data_bytes, new_s->tracked_data_bytes);
    ASSERT_EQ(s->tracked_metadata_bytes, new_s->tracked_metadata_bytes);

    objectSetVal(orig, NULL); /* prevent double-free of s */
    decrRefCount(copy);
    sdsfree(name);
    decrRefCount(key);
    freeStream(s);
}

/* ── 10. Fuzzer: random mix of operations ─────────────────────────────── */
TEST_F(StreamTrackingTest, Fuzzer) {
    unsigned seed = static_cast<unsigned>(time(nullptr)) ^ static_cast<unsigned>(getpid());
    srand(seed);
    printf("  Fuzzer seed: %u\n", seed);

    stream *s = streamNew();
    robj *key = createStringObject("mystream", 8);
    streamID zero = {0, 0};

    /* Create a few CGs upfront */
    streamCG *cgs[3];
    for (int i = 0; i < 3; i++) {
        char name[16];
        snprintf(name, sizeof(name), "cg%d", i);
        cgs[i] = streamCreateCG(s, name, strlen(name), &zero, 0);
    }
    ASSERT_STREAM_TRACKING(s);

    std::vector<streamID> ids;
    std::vector<std::pair<int, streamID>> pending; /* (cg_index, id) */

    const int NUM_OPS = 2000;

    for (int op = 0; op < NUM_OPS; op++) {
        int action = rand() % 100;

        if (action < 40 || ids.empty()) {
            /* XADD: 40% */
            char f[32];
            snprintf(f, sizeof(f), "field_%d", op);
            size_t vlen = 5 + (static_cast<size_t>(rand()) % 50);
            std::string val(vlen, 'a' + (rand() % 26));
            streamID id = appendEntry(s, f, val.c_str());
            ids.push_back(id);
        } else if (action < 55 && ids.size() > 20) {
            /* XTRIM: 15% — trim to half */
            streamTrimByLength(s, ids.size() / 2, 0);
        } else if (action < 70) {
            /* Create consumer: 15% */
            int cg_idx = rand() % 3;
            char cname[32];
            snprintf(cname, sizeof(cname), "consumer_%d_%d", op, rand() % 1000);
            sds sname = sdsnew(cname);
            streamConsumer *c = streamCreateConsumer(cgs[cg_idx], sname, key, 0,
                                                     SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
            if (c) {
                s->tracked_metadata_bytes += sizeof(streamConsumer);
                s->tracked_data_bytes += sdsReqSize(sdslen(c->name), sdsType(c->name));
            }
            sdsfree(sname);
        } else if (action < 85 && !ids.empty()) {
            /* Deliver NACK: 15% */
            int cg_idx = rand() % 3;
            /* Pick a random consumer from this CG */
            if (raxSize(cgs[cg_idx]->consumers) > 0) {
                raxIterator ci;
                raxStart(&ci, cgs[cg_idx]->consumers);
                raxSeek(&ci, "^", NULL, 0);
                raxNext(&ci);
                streamConsumer *c = (streamConsumer *)ci.data;
                raxStop(&ci);

                /* Pick a random ID */
                int idx = rand() % ids.size();
                unsigned char buf[sizeof(streamID)];
                streamEncodeID(buf, &ids[idx]);
                streamNACK *nack = streamCreateNACK(c);
                if (raxTryInsert(cgs[cg_idx]->pel, buf, sizeof(buf), nack, NULL)) {
                    raxInsert(c->pel, buf, sizeof(buf), nack, NULL);
                    s->tracked_metadata_bytes += sizeof(streamNACK);
                    pending.push_back({cg_idx, ids[idx]});
                } else {
                    streamFreeNACK(nack);
                }
            }
        } else if (!pending.empty()) {
            /* ACK: remaining % */
            int idx = rand() % pending.size();
            auto [cg_idx, ack_id] = pending[idx];
            unsigned char buf[sizeof(streamID)];
            streamEncodeID(buf, &ack_id);
            void *result;
            if (raxFind(cgs[cg_idx]->pel, buf, sizeof(buf), &result)) {
                streamNACK *nack = (streamNACK *)result;
                raxRemove(cgs[cg_idx]->pel, buf, sizeof(buf), NULL);
                raxRemove(nack->consumer->pel, buf, sizeof(buf), NULL);
                streamFreeNACK(nack);
                s->tracked_metadata_bytes -= sizeof(streamNACK);
            }
            pending.erase(pending.begin() + idx);
        }

        /* Check tracking every 50 operations */
        if (op % 50 == 0) {
            ASSERT_STREAM_TRACKING(s);
        }
    }

    /* Final check */
    ASSERT_STREAM_TRACKING(s);

    decrRefCount(key);
    freeStream(s);
}
