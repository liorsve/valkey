/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Unit tests for stream tracked_data_bytes and tracked_overhead.
 */

#include "../fmacros.h"
#include "../server.h"
#include "../stream.h"
#include "../rax.h"
#include "../listpack.h"
#include "../sds.h"
#include "test_help.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* ── Forward declarations for internal functions ─────────────────────── */
int streamAppendItem(stream *s, robj **argv, int64_t numfields, streamID *added_id, streamID *use_id, int seq_given);
streamCG *streamCreateCG(stream *s, char *name, size_t namelen, streamID *id, long long entries_read);
streamConsumer *streamCreateConsumer(streamCG *cg, sds name, robj *key, int dbid, int flags);
streamNACK *streamCreateNACK(streamConsumer *consumer);
void streamFreeNACK(streamNACK *na);
void streamDelConsumer(streamCG *cg, streamConsumer *consumer);
void freeStream(stream *s);
int64_t streamTrimByLength(stream *s, long long maxlen, int approx);
int64_t streamTrimByID(stream *s, streamID minid, int approx);
void streamEncodeID(void *buf, streamID *id);
robj *streamDup(robj *o);

/* ── Ground truth: walk everything and compute sizes ─────────────────── */

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
            streamCG *cg = ri.data;
            raxIterator ci;
            raxStart(&ci, cg->consumers);
            raxSeek(&ci, "^", NULL, 0);
            while (raxNext(&ci)) {
                streamConsumer *sc = ci.data;
                total += sdsReqSize(sdslen(sc->name), sdsType(sc->name));
            }
            raxStop(&ci);
        }
        raxStop(&ri);
    }
    return total;
}

static size_t computeOverheadWalk(stream *s) {
    size_t total = 0;
    total += raxLogicalSize(s->rax);
    if (s->cgroups) {
        total += raxLogicalSize(s->cgroups);
        raxIterator ri;
        raxStart(&ri, s->cgroups);
        raxSeek(&ri, "^", NULL, 0);
        while (raxNext(&ri)) {
            streamCG *cg = ri.data;
            total += sizeof(streamCG);
            total += raxLogicalSize(cg->pel);
            total += raxSize(cg->pel) * sizeof(streamNACK);
            total += raxLogicalSize(cg->consumers);
            raxIterator ci;
            raxStart(&ci, cg->consumers);
            raxSeek(&ci, "^", NULL, 0);
            while (raxNext(&ci)) {
                streamConsumer *sc = ci.data;
                total += sizeof(streamConsumer);
                total += raxLogicalSize(sc->pel);
            }
            raxStop(&ci);
        }
        raxStop(&ri);
    }
    return total;
}

#define ASSERT_STREAM_TRACKING(s)                                               \
    do {                                                                        \
        size_t wd = computeDataBytesWalk(s);                                    \
        size_t wo = computeOverheadWalk(s);                                     \
        if ((s)->tracked_data_bytes != wd) {                                    \
            fprintf(stderr, "tracked_data_bytes mismatch: tracked=%zu walk=%zu at %s:%d\n", \
                   (s)->tracked_data_bytes, wd, __FILE__, __LINE__);            \
            TEST_ASSERT(0);                                                     \
        }                                                                       \
        if ((s)->tracked_overhead != wo) {                                      \
            fprintf(stderr, "tracked_overhead mismatch: tracked=%zu walk=%zu diff=%zd at %s:%d\n", \
                   (s)->tracked_overhead, wo,                                   \
                   (ssize_t)((s)->tracked_overhead - wo), __FILE__, __LINE__);  \
            TEST_ASSERT(0);                                                     \
        }                                                                       \
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

int test_stream_tracking_append(int argc, char **argv, int flags) {
    UNUSED(argc); UNUSED(argv); UNUSED(flags);
    stream *s = streamNew();
    server.stream_node_max_entries = 10;

    for (int i = 0; i < 100; i++) {
        char f[16], v[32];
        snprintf(f, sizeof(f), "f%d", i);
        snprintf(v, sizeof(v), "value_%d_data", i);
        appendEntry(s, f, v);
        ASSERT_STREAM_TRACKING(s);
    }
    /* With max_entries=10, 100 entries must span multiple rax nodes */
    TEST_ASSERT(raxSize(s->rax) == 10);

    server.stream_node_max_entries = 100;
    freeStream(s);
    return 0;
}

int test_stream_tracking_trim(int argc, char **argv, int flags) {
    UNUSED(argc); UNUSED(argv); UNUSED(flags);
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
    return 0;
}

int test_stream_tracking_trim_by_id(int argc, char **argv, int flags) {
    UNUSED(argc); UNUSED(argv); UNUSED(flags);
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
    TEST_ASSERT(raxSize(s->rax) == 5);

    streamTrimByID(s, ids[35], 0);
    ASSERT_STREAM_TRACKING(s);
    TEST_ASSERT(raxSize(s->rax) < 5);

    server.stream_node_max_entries = 100;
    freeStream(s);
    return 0;
}

/* Listpack stores integers in variable-width encoding: values 0-127 use
 * 7-bit (1 byte), 128+ use 13-bit (2 bytes). The "valid entries" counter
 * crosses 128→127 during trim, causing a 1-byte lpBytes change. */
int test_stream_tracking_trim_encoding_boundary(int argc, char **argv, int flags) {
    UNUSED(argc); UNUSED(argv); UNUSED(flags);
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
    TEST_ASSERT(raxSize(s->rax) == 1);

    size_t bytes_before = s->tracked_data_bytes;
    streamTrimByLength(s, 127, 0);
    ASSERT_STREAM_TRACKING(s);
    /* Counter crossed 128→127, encoding change causes 1-byte decrease */
    TEST_ASSERT(bytes_before - s->tracked_data_bytes == 1);

    server.stream_node_max_entries = 100;
    server.stream_node_max_bytes = 4096;
    freeStream(s);
    return 0;
}

int test_stream_tracking_iterator_remove(int argc, char **argv, int flags) {
    UNUSED(argc); UNUSED(argv); UNUSED(flags);
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
    TEST_ASSERT(s->tracked_data_bytes == 0);

    freeStream(s);
    return 0;
}

int test_stream_tracking_cg_create(int argc, char **argv, int flags) {
    UNUSED(argc); UNUSED(argv); UNUSED(flags);
    stream *s = streamNew();
    appendEntry(s, "f", "v");

    streamID zero = {0, 0};
    streamCreateCG(s, (char *)"grp1", 4, &zero, 0);
    streamCreateCG(s, (char *)"grp2", 4, &zero, 0);
    ASSERT_STREAM_TRACKING(s);

    freeStream(s);
    return 0;
}

int test_stream_tracking_full_lifecycle(int argc, char **argv, int flags) {
    UNUSED(argc); UNUSED(argv); UNUSED(flags);
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

    /* Create consumers — mirrors command handler tracking. */
    robj *key = createStringObject("mystream", 8);
    sds name1 = sdsnew("alice");
    sds name2 = sdsnew("bob_with_longer_name");
    streamConsumer *c1 = streamCreateConsumer(cg, name1, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    if (c1) {
        s->tracked_overhead += sizeof(streamConsumer);
        s->tracked_data_bytes += sdsReqSize(sdslen(c1->name), sdsType(c1->name));
        raxSetExternalLogicalSize(c1->pel, &s->tracked_overhead);
    }
    streamConsumer *c2 = streamCreateConsumer(cg, name2, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    if (c2) {
        s->tracked_overhead += sizeof(streamConsumer);
        s->tracked_data_bytes += sdsReqSize(sdslen(c2->name), sdsType(c2->name));
        raxSetExternalLogicalSize(c2->pel, &s->tracked_overhead);
    }
    ASSERT_STREAM_TRACKING(s);

    /* Deliver NACKs — mirrors streamReplyWithRange (XREADGROUP). */
    for (int i = 0; i < 10; i++) {
        streamNACK *nack = streamCreateNACK(c1);
        unsigned char buf[sizeof(streamID)];
        streamEncodeID(buf, &ids[i]);
        raxInsert(cg->pel, buf, sizeof(buf), nack, NULL);
        raxInsert(c1->pel, buf, sizeof(buf), nack, NULL);
        s->tracked_overhead += sizeof(streamNACK);
    }
    ASSERT_STREAM_TRACKING(s);

    /* ACK 5 NACKs — mirrors xackCommand. */
    for (int i = 0; i < 5; i++) {
        unsigned char buf[sizeof(streamID)];
        streamEncodeID(buf, &ids[i]);
        void *result;
        raxFind(cg->pel, buf, sizeof(buf), &result);
        raxRemove(cg->pel, buf, sizeof(buf), NULL);
        raxRemove(c1->pel, buf, sizeof(buf), NULL);
        streamFreeNACK((streamNACK *)result);
        s->tracked_overhead -= sizeof(streamNACK);
    }
    ASSERT_STREAM_TRACKING(s);

    /* Delete consumer c2 (0 NACKs) — mirrors DELCONSUMER. */
    s->tracked_overhead -= sizeof(streamConsumer);
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
    return 0;
}

int test_stream_tracking_destroy_cg(int argc, char **argv, int flags) {
    UNUSED(argc); UNUSED(argv); UNUSED(flags);
    stream *s = streamNew();
    appendEntry(s, "f", "v");

    streamID zero = {0, 0};
    streamCreateCG(s, (char *)"grp1", 4, &zero, 0);
    streamCG *cg2 = streamCreateCG(s, (char *)"grp2", 4, &zero, 0);

    /* Create consumers — mirrors command handler tracking. */
    robj *key = createStringObject("mystream", 8);
    sds name1 = sdsnew("worker_alpha");
    sds name2 = sdsnew("worker_beta_longer");
    streamConsumer *c1 = streamCreateConsumer(cg2, name1, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    s->tracked_overhead += sizeof(streamConsumer);
    s->tracked_data_bytes += sdsReqSize(sdslen(c1->name), sdsType(c1->name));
    raxSetExternalLogicalSize(c1->pel, &s->tracked_overhead);
    streamConsumer *c2 = streamCreateConsumer(cg2, name2, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    s->tracked_overhead += sizeof(streamConsumer);
    s->tracked_data_bytes += sdsReqSize(sdslen(c2->name), sdsType(c2->name));
    raxSetExternalLogicalSize(c2->pel, &s->tracked_overhead);

    streamID ids[10];
    for (int i = 0; i < 10; i++) {
        char f[16], v[16];
        snprintf(f, sizeof(f), "f%d", i);
        snprintf(v, sizeof(v), "v%d", i);
        ids[i] = appendEntry(s, f, v);
    }
    /* Deliver NACKs — mirrors streamReplyWithRange. */
    for (int i = 0; i < 7; i++) {
        streamNACK *nack = streamCreateNACK(i < 4 ? c1 : c2);
        unsigned char buf[sizeof(streamID)];
        streamEncodeID(buf, &ids[i]);
        streamConsumer *target = (i < 4 ? c1 : c2);
        raxInsert(cg2->pel, buf, sizeof(buf), nack, NULL);
        raxInsert(target->pel, buf, sizeof(buf), nack, NULL);
        s->tracked_overhead += sizeof(streamNACK);
    }
    ASSERT_STREAM_TRACKING(s);

    /* Destroy cg2 — mirrors xgroupCommand DESTROY. */
    raxRemove(s->cgroups, (unsigned char *)"grp2", 4, NULL);
    s->tracked_overhead -= sizeof(streamCG);
    raxFreeWithCallbackAndContext(cg2->pel, streamFreeNACKWithTracking, s);
    raxFreeWithCallbackAndContext(cg2->consumers, streamFreeConsumerWithTracking, s);
    zfree(cg2);
    ASSERT_STREAM_TRACKING(s);

    /* grp1 still exists */
    TEST_ASSERT(s->tracked_overhead > 0);

    sdsfree(name1);
    sdsfree(name2);
    decrRefCount(key);
    freeStream(s);
    return 0;
}

int test_stream_tracking_del_consumer(int argc, char **argv, int flags) {
    UNUSED(argc); UNUSED(argv); UNUSED(flags);
    stream *s = streamNew();
    appendEntry(s, "f", "v");

    streamID zero = {0, 0};
    streamCG *cg = streamCreateCG(s, (char *)"grp", 3, &zero, 0);

    /* Create consumer — mirrors command handler tracking. */
    robj *key = createStringObject("mystream", 8);
    sds name = sdsnew("busy_consumer");
    streamConsumer *consumer = streamCreateConsumer(cg, name, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    s->tracked_overhead += sizeof(streamConsumer);
    s->tracked_data_bytes += sdsReqSize(sdslen(consumer->name), sdsType(consumer->name));
    raxSetExternalLogicalSize(consumer->pel, &s->tracked_overhead);

    /* Deliver 8 NACKs — mirrors streamReplyWithRange. */
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
        s->tracked_overhead += sizeof(streamNACK);
    }
    ASSERT_STREAM_TRACKING(s);

    /* Delete consumer — mirrors xgroupCommand DELCONSUMER. */
    long long pending = raxSize(consumer->pel);
    s->tracked_overhead -= sizeof(streamConsumer) + pending * sizeof(streamNACK);
    s->tracked_data_bytes -= sdsReqSize(sdslen(consumer->name), sdsType(consumer->name));
    streamDelConsumer(cg, consumer);
    ASSERT_STREAM_TRACKING(s);

    sdsfree(name);
    decrRefCount(key);
    freeStream(s);
    return 0;
}

int test_stream_tracking_dup(int argc, char **argv, int flags) {
    UNUSED(argc); UNUSED(argv); UNUSED(flags);
    stream *s = streamNew();

    for (int i = 0; i < 20; i++) {
        char f[16], v[32];
        snprintf(f, sizeof(f), "f%d", i);
        snprintf(v, sizeof(v), "value_%d", i);
        appendEntry(s, f, v);
    }

    streamID zero = {0, 0};
    streamCG *cg = streamCreateCG(s, (char *)"grp", 3, &zero, 0);

    /* Create consumer — mirrors command handler tracking. */
    robj *key = createStringObject("mystream", 8);
    sds name = sdsnew("consumer_one");
    streamConsumer *consumer = streamCreateConsumer(cg, name, key, 0, SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
    s->tracked_overhead += sizeof(streamConsumer);
    s->tracked_data_bytes += sdsReqSize(sdslen(consumer->name), sdsType(consumer->name));
    raxSetExternalLogicalSize(consumer->pel, &s->tracked_overhead);

    /* Deliver NACKs — mirrors streamReplyWithRange. */
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
        s->tracked_overhead += sizeof(streamNACK);
    }
    ASSERT_STREAM_TRACKING(s);

    /* Duplicate */
    robj *orig = createStreamObject();
    freeStream(objectGetVal(orig));
    objectSetVal(orig, s);

    robj *copy = streamDup(orig);
    stream *new_s = objectGetVal(copy);

    ASSERT_STREAM_TRACKING(new_s);
    TEST_ASSERT(s->tracked_data_bytes == new_s->tracked_data_bytes);
    TEST_ASSERT(s->tracked_overhead == new_s->tracked_overhead);

    objectSetVal(orig, NULL); /* prevent double-free of s */
    decrRefCount(orig);
    decrRefCount(copy);
    sdsfree(name);
    decrRefCount(key);
    freeStream(s);
    return 0;
}

int test_stream_tracking_fuzzer(int argc, char **argv, int flags) {
    UNUSED(argc); UNUSED(argv); UNUSED(flags);
    unsigned seed = (unsigned)time(NULL) ^ (unsigned)getpid();
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

    streamID ids[2048];
    int id_count = 0;
    typedef struct { int cg_idx; streamID id; } pending_t;
    pending_t pending[4096];
    int pending_count = 0;

    const int NUM_OPS = 2000;
    for (int op = 0; op < NUM_OPS; op++) {
        int action = rand() % 100;

        if (action < 40 || id_count == 0) {
            /* XADD */
            if (id_count < 2048) {
                char f[32];
                snprintf(f, sizeof(f), "field_%d", op);
                size_t vlen = 5 + ((size_t)rand() % 50);
                char val[64];
                memset(val, 'a' + (rand() % 26), vlen);
                val[vlen] = '\0';
                ids[id_count] = appendEntry(s, f, val);
                id_count++;
            }
        } else if (action < 55 && id_count > 20) {
            /* XTRIM */
            streamTrimByLength(s, id_count / 2, 0);
        } else if (action < 70) {
            /* Create consumer — mirrors command handler tracking. */
            int cg_idx = rand() % 3;
            char cname[32];
            snprintf(cname, sizeof(cname), "consumer_%d_%d", op, rand() % 1000);
            sds sname = sdsnew(cname);
            streamConsumer *c = streamCreateConsumer(cgs[cg_idx], sname, key, 0,
                                                     SCC_NO_NOTIFY | SCC_NO_DIRTIFY);
            if (c) {
                s->tracked_overhead += sizeof(streamConsumer);
                s->tracked_data_bytes += sdsReqSize(sdslen(c->name), sdsType(c->name));
                raxSetExternalLogicalSize(c->pel, &s->tracked_overhead);
            }
            sdsfree(sname);
        } else if (action < 85 && id_count > 0) {
            /* Deliver NACK — mirrors streamReplyWithRange. */
            int cg_idx = rand() % 3;
            if (raxSize(cgs[cg_idx]->consumers) > 0) {
                raxIterator ci;
                raxStart(&ci, cgs[cg_idx]->consumers);
                raxSeek(&ci, "^", NULL, 0);
                raxNext(&ci);
                streamConsumer *c = ci.data;
                raxStop(&ci);

                int idx = rand() % id_count;
                unsigned char buf[sizeof(streamID)];
                streamEncodeID(buf, &ids[idx]);
                streamNACK *nack = streamCreateNACK(c);
                if (raxTryInsert(cgs[cg_idx]->pel, buf, sizeof(buf), nack, NULL)) {
                    raxInsert(c->pel, buf, sizeof(buf), nack, NULL);
                    s->tracked_overhead += sizeof(streamNACK);
                    if (pending_count < 4096) {
                        pending[pending_count].cg_idx = cg_idx;
                        pending[pending_count].id = ids[idx];
                        pending_count++;
                    }
                } else {
                    streamFreeNACK(nack);
                }
            }
        } else if (pending_count > 0) {
            /* ACK — mirrors xackCommand. */
            int idx = rand() % pending_count;
            int cg_idx = pending[idx].cg_idx;
            unsigned char buf[sizeof(streamID)];
            streamEncodeID(buf, &pending[idx].id);
            void *result;
            if (raxFind(cgs[cg_idx]->pel, buf, sizeof(buf), &result)) {
                streamNACK *nack = result;
                streamConsumer *nack_consumer = nack->consumer;
                raxRemove(cgs[cg_idx]->pel, buf, sizeof(buf), NULL);
                raxRemove(nack_consumer->pel, buf, sizeof(buf), NULL);
                streamFreeNACK(nack);
                s->tracked_overhead -= sizeof(streamNACK);
            }
            pending[idx] = pending[--pending_count];
        }

        if (op % 50 == 0) ASSERT_STREAM_TRACKING(s);
    }
    ASSERT_STREAM_TRACKING(s);

    decrRefCount(key);
    freeStream(s);
    return 0;
}
