/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Correctness tests for HASH hashtable tracked_data_bytes.
 * Covers every mutation path: add new field, update existing value,
 * set/remove expiry, conversion (listpack→ht), dup, delete, empty,
 * and a fuzzer for randomized coverage.
 */

#include "generated_wrappers.hpp"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

extern "C" {
#include "entry.h"
#include "expire.h"
#include "hashtable.h"
#include "sds.h"
#include "server.h"
#include "zmalloc.h"
}

class HashTrackingTest : public ::testing::Test {};

/* ── Ground-truth helper ────────────────────────────────────────────── */

/* Independently compute the logical size of a single entry from raw accessors,
 * without calling entryGetLogicalSize — so the ground truth is independent of
 * the function under test. */
static size_t computeEntrySize(const entry *e) {
    sds field = entryGetField(e);
    size_t size = sdsHdrSize(sdsType(field)) + sdslen(field) + 1;

    if (entryHasExpiry(e)) size += sizeof(mstime_t);

    if (entryHasEmbeddedValue(e)) {
        size_t vlen;
        char *val = entryGetValue(e, &vlen);
        size += sdsHdrSize(sdsType((sds)val)) + vlen + 1;
    } else {
        size += sizeof(void *);
        if (entryHasStringRef(e)) {
            size += sizeof(stringRef);
        } else {
            size_t vlen;
            char *val = entryGetValue(e, &vlen);
            size += sdsHdrSize(sdsType((sds)val)) + vlen + 1;
        }
    }
    return size;
}

/* Walk all entries and sum independently-computed logical sizes. */
static size_t computeExpectedDataBytes(hashtable *ht) {
    size_t total = 0;
    hashtableIterator iter;
    hashtableInitIterator(&iter, ht, HASHTABLE_ITER_SKIP_VALIDATION);
    void *e;
    while (hashtableNext(&iter, &e)) {
        total += computeEntrySize(static_cast<const entry *>(e));
    }
    hashtableCleanupIterator(&iter);
    return total;
}

#define ASSERT_TRACKED_CORRECT(ht)                          \
    do {                                                    \
        size_t _expected = computeExpectedDataBytes(ht);    \
        ASSERT_EQ(hashtableTrackedDataBytes(ht), _expected) \
            << "tracked_data_bytes mismatch";               \
    } while (0)

/* ── Helpers ────────────────────────────────────────────────────────── */

/* Create a hashtable-encoded hash object. */
static robj *createTestHash(void) {
    robj *o = createHashObject();
    hashTypeConvert(o, OBJ_ENCODING_HASHTABLE);
    return o;
}

/* Add or update field/value in a hash, no expiry. */
static void hashSet(robj *o, const char *field, const char *value) {
    sds f = sdsnew(field);
    sds v = sdsnew(value);
    hashTypeSet(o, f, v, EXPIRY_NONE, HASH_SET_TAKE_FIELD | HASH_SET_TAKE_VALUE, NULL);
}

/* Add or update field/value with expiry. */
static void hashSetExpiry(robj *o, const char *field, const char *value, mstime_t expiry) {
    sds f = sdsnew(field);
    sds v = sdsnew(value);
    hashTypeSet(o, f, v, expiry, HASH_SET_TAKE_FIELD | HASH_SET_TAKE_VALUE, NULL);
}

/* Delete a field from a hash. */
static bool hashDel(robj *o, const char *field) {
    sds f = sdsnew(field);
    bool deleted = hashTypeDelete(o, f);
    sdsfree(f);
    return deleted;
}

/* ── 1. Add new fields ──────────────────────────────────────────────── */
TEST_F(HashTrackingTest, AddNewFields) {
    robj *o = createTestHash();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(o));

    for (int i = 0; i < 100; i++) {
        char f[32], v[64];
        snprintf(f, sizeof(f), "field_%d", i);
        snprintf(v, sizeof(v), "value_%d_%0*d", i, 30, i);
        hashSet(o, f, v);
        ASSERT_TRACKED_CORRECT(ht);
    }

    decrRefCount(o);
}

/* ── 2. Update existing field with different value size ─────────────── */
TEST_F(HashTrackingTest, UpdateExistingValue) {
    robj *o = createTestHash();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(o));

    hashSet(o, "key", "short");
    ASSERT_TRACKED_CORRECT(ht);

    /* Update with a longer value */
    hashSet(o, "key", "a_much_longer_value_that_changes_the_logical_size");
    ASSERT_TRACKED_CORRECT(ht);

    /* Update with a shorter value */
    hashSet(o, "key", "x");
    ASSERT_TRACKED_CORRECT(ht);

    decrRefCount(o);
}

/* ── 3. Set expiry on a field (adds sizeof(mstime_t) to entry) ──────── */
TEST_F(HashTrackingTest, SetExpiry) {
    robj *o = createTestHash();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(o));

    /* Add without expiry */
    hashSet(o, "myfield", "myvalue");
    size_t without_expiry = hashtableTrackedDataBytes(ht);
    ASSERT_TRACKED_CORRECT(ht);

    /* Update same field with expiry. Entry transitions from Type 1 (SDS_TYPE_5
     * field, no expiry) to Type 2 (SDS_TYPE_8 field, with expiry).
     * Diff = sizeof(mstime_t) + (sdsHdrSize(SDS_TYPE_8) - sdsHdrSize(SDS_TYPE_5)) */
    hashSetExpiry(o, "myfield", "myvalue", mstime() + 100000);
    size_t with_expiry = hashtableTrackedDataBytes(ht);
    ASSERT_TRACKED_CORRECT(ht);
    size_t expected_diff = sizeof(mstime_t) +
                           (static_cast<size_t>(sdsHdrSize(SDS_TYPE_8)) - static_cast<size_t>(sdsHdrSize(SDS_TYPE_5)));
    ASSERT_EQ(with_expiry - without_expiry, expected_diff);

    decrRefCount(o);
}

/* ── 4. Update value AND toggle expiry simultaneously ───────────────── */
TEST_F(HashTrackingTest, UpdateValueAndExpiry) {
    robj *o = createTestHash();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(o));

    hashSet(o, "f1", "val_no_exp");
    ASSERT_TRACKED_CORRECT(ht);

    /* Add expiry + change value */
    hashSetExpiry(o, "f1", "new_val_with_exp", mstime() + 100000);
    ASSERT_TRACKED_CORRECT(ht);

    /* Remove expiry by setting again without */
    hashSet(o, "f1", "back_to_no_exp");
    ASSERT_TRACKED_CORRECT(ht);

    decrRefCount(o);
}

/* ── 5. Delete fields ───────────────────────────────────────────────── */
TEST_F(HashTrackingTest, DeleteFields) {
    robj *o = createTestHash();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(o));

    for (int i = 0; i < 50; i++) {
        char f[32], v[32];
        snprintf(f, sizeof(f), "f_%d", i);
        snprintf(v, sizeof(v), "v_%d", i);
        hashSet(o, f, v);
    }
    ASSERT_TRACKED_CORRECT(ht);

    for (int i = 0; i < 50; i++) {
        char f[32];
        snprintf(f, sizeof(f), "f_%d", i);
        ASSERT_TRUE(hashDel(o, f));
        ASSERT_TRACKED_CORRECT(ht);
    }
    ASSERT_EQ(hashtableTrackedDataBytes(ht), 0ul);

    decrRefCount(o);
}

/* ── 6. Delete nonexistent field ────────────────────────────────────── */
TEST_F(HashTrackingTest, DeleteNonexistent) {
    robj *o = createTestHash();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(o));

    hashSet(o, "a", "b");
    size_t before = hashtableTrackedDataBytes(ht);

    ASSERT_FALSE(hashDel(o, "nonexistent"));
    ASSERT_EQ(hashtableTrackedDataBytes(ht), before);
    ASSERT_TRACKED_CORRECT(ht);

    decrRefCount(o);
}

/* ── 7. Duplicate add does not double-count ─────────────────────────── */
TEST_F(HashTrackingTest, DuplicateFieldSameValue) {
    robj *o = createTestHash();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(o));

    hashSet(o, "dup", "val");
    ASSERT_TRACKED_CORRECT(ht);

    /* Set same field with same value — entry updated in place */
    hashSet(o, "dup", "val");
    ASSERT_TRACKED_CORRECT(ht);

    decrRefCount(o);
}

/* ── 8. Various field/value sizes (SDS TYPE_5, TYPE_8, TYPE_16) ─────── */
TEST_F(HashTrackingTest, VariousSizes) {
    robj *o = createTestHash();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(o));

    /* Small (TYPE_5 field) */
    hashSet(o, "f", "v");
    ASSERT_TRACKED_CORRECT(ht);

    /* Medium (TYPE_8) */
    std::string med_f(100, 'F');
    std::string med_v(200, 'V');
    hashSet(o, med_f.c_str(), med_v.c_str());
    ASSERT_TRACKED_CORRECT(ht);

    /* Large (TYPE_16) */
    std::string big_f(300, 'G');
    std::string big_v(500, 'W');
    hashSet(o, big_f.c_str(), big_v.c_str());
    ASSERT_TRACKED_CORRECT(ht);

    decrRefCount(o);
}

/* ── 9. Conversion: listpack → hashtable ────────────────────────────── */
TEST_F(HashTrackingTest, ConvertListpackToHashtable) {
    size_t saved = server.hash_max_listpack_entries;
    size_t saved_val = server.hash_max_listpack_value;
    server.hash_max_listpack_entries = 5;
    server.hash_max_listpack_value = 64;

    robj *o = createHashObject();

    /* Add within listpack limit */
    for (int i = 0; i < 5; i++) {
        char f[32], v[32];
        snprintf(f, sizeof(f), "lf_%d", i);
        snprintf(v, sizeof(v), "lv_%d", i);
        hashSet(o, f, v);
    }
    ASSERT_EQ(o->encoding, static_cast<unsigned int>(OBJ_ENCODING_LISTPACK));

    /* One more triggers conversion */
    hashSet(o, "overflow_f", "overflow_v");
    ASSERT_EQ(o->encoding, static_cast<unsigned int>(OBJ_ENCODING_HASHTABLE));

    hashtable *ht = static_cast<hashtable *>(objectGetVal(o));
    ASSERT_TRACKED_CORRECT(ht);

    server.hash_max_listpack_entries = saved;
    server.hash_max_listpack_value = saved_val;
    decrRefCount(o);
}

/* ── 10. hashTypeDup (COPY command path) ────────────────────────────── */
TEST_F(HashTrackingTest, HashTypeDup) {
    robj *o = createTestHash();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(o));

    for (int i = 0; i < 30; i++) {
        char f[32], v[32];
        snprintf(f, sizeof(f), "df_%d", i);
        snprintf(v, sizeof(v), "dv_%d", i);
        hashSet(o, f, v);
    }
    ASSERT_TRACKED_CORRECT(ht);

    robj *copy = hashTypeDup(o);
    hashtable *ht_copy = static_cast<hashtable *>(objectGetVal(copy));
    ASSERT_TRACKED_CORRECT(ht_copy);
    ASSERT_EQ(hashtableTrackedDataBytes(ht), hashtableTrackedDataBytes(ht_copy));

    decrRefCount(o);
    decrRefCount(copy);
}

/* ── 11. hashtableEmpty resets tracking ──────────────────────────────── */
TEST_F(HashTrackingTest, HashtableEmpty) {
    robj *o = createTestHash();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(o));

    for (int i = 0; i < 50; i++) {
        char f[32], v[32];
        snprintf(f, sizeof(f), "e_%d", i);
        snprintf(v, sizeof(v), "ev_%d", i);
        hashSet(o, f, v);
    }
    ASSERT_TRACKED_CORRECT(ht);

    hashtableEmpty(ht, nullptr);
    ASSERT_EQ(hashtableTrackedDataBytes(ht), 0ul);

    decrRefCount(o);
}

/* ── 12. Interleaved add/update/delete ──────────────────────────────── */
TEST_F(HashTrackingTest, InterleavedOperations) {
    robj *o = createTestHash();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(o));

    /* Add 200 fields */
    for (int i = 0; i < 200; i++) {
        char f[32], v[32];
        snprintf(f, sizeof(f), "k_%04d", i);
        snprintf(v, sizeof(v), "v_%04d", i);
        hashSet(o, f, v);
    }
    ASSERT_TRACKED_CORRECT(ht);

    /* Update every other field with a longer value */
    for (int i = 0; i < 200; i += 2) {
        char f[32], v[128];
        snprintf(f, sizeof(f), "k_%04d", i);
        memset(v, 'X', 100);
        snprintf(v + 100, sizeof(v) - 100, "_%d", i);
        hashSet(o, f, v);
    }
    ASSERT_TRACKED_CORRECT(ht);

    /* Delete every third field */
    for (int i = 0; i < 200; i += 3) {
        char f[32];
        snprintf(f, sizeof(f), "k_%04d", i);
        hashDel(o, f);
    }
    ASSERT_TRACKED_CORRECT(ht);

    decrRefCount(o);
}

/* ── 13. Empty hash: zero tracked bytes ─────────────────────────────── */
TEST_F(HashTrackingTest, EmptyHashZeroBytes) {
    robj *o = createTestHash();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(o));

    ASSERT_EQ(hashtableTrackedDataBytes(ht), 0ul);
    ASSERT_TRACKED_CORRECT(ht);

    decrRefCount(o);
}

/* ── 14. entrySetExpiry: add expiry without value change (hashTypeSetExpire path)
 *     hashTypeSetExpire is static, so we simulate its logic directly. */
TEST_F(HashTrackingTest, EntrySetExpiryAddExpiry) {
    robj *o = createTestHash();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(o));

    /* Add a field without expiry */
    hashSet(o, "persist_field", "some_value");
    ASSERT_TRACKED_CORRECT(ht);

    /* Simulate hashTypeSetExpire: find entry, set expiry, adjust tracking */
    sds field = sdsnew("persist_field");
    void **eref = hashtableFindRef(ht, field);
    ASSERT_NE(eref, nullptr);
    entry *e = static_cast<entry *>(*eref);
    ASSERT_FALSE(entryHasExpiry(e));

    size_t before = hashtableTrackedDataBytes(ht);
    size_t old_size = entryGetLogicalSize(e);
    *eref = entrySetExpiry(e, mstime() + 100000);
    hashtableAdjustTrackedDataBytes(ht, static_cast<ssize_t>(entryGetLogicalSize(static_cast<entry *>(*eref))) -
                                            static_cast<ssize_t>(old_size));
    ASSERT_TRACKED_CORRECT(ht);
    ASSERT_GT(hashtableTrackedDataBytes(ht), before);

    sdsfree(field);
    decrRefCount(o);
}

/* ── 15. entrySetExpiry: remove expiry (hashTypePersist path)
 *     hashTypePersist is static, so we simulate its logic directly. */
TEST_F(HashTrackingTest, EntrySetExpiryRemoveExpiry) {
    robj *o = createTestHash();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(o));

    /* Add a field with expiry */
    hashSetExpiry(o, "exp_field", "exp_value", mstime() + 100000);
    ASSERT_TRACKED_CORRECT(ht);

    /* Simulate hashTypePersist: find entry, remove expiry, adjust tracking */
    sds field = sdsnew("exp_field");
    void **eref = hashtableFindRef(ht, field);
    ASSERT_NE(eref, nullptr);
    entry *e = static_cast<entry *>(*eref);
    ASSERT_TRUE(entryHasExpiry(e));
    size_t with_expiry = hashtableTrackedDataBytes(ht);

    size_t old_size = entryGetLogicalSize(e);
    *eref = entrySetExpiry(e, EXPIRY_NONE);
    hashtableAdjustTrackedDataBytes(ht, static_cast<ssize_t>(entryGetLogicalSize(static_cast<entry *>(*eref))) -
                                            static_cast<ssize_t>(old_size));
    ASSERT_TRACKED_CORRECT(ht);
    ASSERT_LT(hashtableTrackedDataBytes(ht), with_expiry);

    sdsfree(field);
    decrRefCount(o);
}

/* ── 16. hashTypeUpdateAsStringRef (stringRef value path) ───────────── */
TEST_F(HashTrackingTest, UpdateAsStringRef) {
    robj *o = createTestHash();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(o));

    /* Add a regular field first */
    hashSet(o, "ref_field", "initial_value");
    ASSERT_TRACKED_CORRECT(ht);

    /* Update to a stringRef value */
    const char *buf = "externalized_buffer_data";
    sds field = sdsnew("ref_field");
    hashTypeUpdateAsStringRef(o, field, buf, strlen(buf));
    sdsfree(field);
    ASSERT_TRACKED_CORRECT(ht);

    /* Update stringRef with different length */
    const char *buf2 = "short";
    field = sdsnew("ref_field");
    hashTypeUpdateAsStringRef(o, field, buf2, strlen(buf2));
    sdsfree(field);
    ASSERT_TRACKED_CORRECT(ht);

    decrRefCount(o);
}

/* ── 17. Fuzzer from listpack (triggers conversion mid-run) ─────────── */
TEST_F(HashTrackingTest, Fuzzer) {
    size_t saved = server.hash_max_listpack_entries;
    size_t saved_val = server.hash_max_listpack_value;
    server.hash_max_listpack_entries = 20;
    server.hash_max_listpack_value = 64;

    robj *o = createHashObject();

    unsigned seed = static_cast<unsigned>(time(nullptr)) ^ static_cast<unsigned>(getpid());
    srand(seed);
    printf("  Fuzzer seed: %u\n", seed);

    std::vector<std::string> fields;
    bool converted = false;

    const int NUM_OPS = 5000;

    for (int op = 0; op < NUM_OPS; op++) {
        int action = rand() % 100;

        if (action < 45 || fields.empty()) {
            /* ADD new field: 45% */
            size_t flen = 1 + (static_cast<size_t>(rand()) % 100);
            size_t vlen = 1 + (static_cast<size_t>(rand()) % 300);
            std::string f(flen, '\0');
            std::string v(vlen, '\0');
            for (size_t j = 0; j < flen; j++) f[j] = static_cast<char>('a' + (rand() % 26));
            for (size_t j = 0; j < vlen; j++) v[j] = static_cast<char>('A' + (rand() % 26));
            hashSet(o, f.c_str(), v.c_str());
            fields.push_back(f);
        } else if (action < 65) {
            /* UPDATE existing field with new value: 20% */
            size_t idx = static_cast<size_t>(rand()) % fields.size();
            size_t vlen = 1 + (static_cast<size_t>(rand()) % 300);
            std::string v(vlen, '\0');
            for (size_t j = 0; j < vlen; j++) v[j] = static_cast<char>('0' + (rand() % 10));
            hashSet(o, fields[idx].c_str(), v.c_str());
        } else if (action < 75) {
            /* SET with expiry: 10% */
            size_t idx = static_cast<size_t>(rand()) % fields.size();
            size_t vlen = 1 + (static_cast<size_t>(rand()) % 100);
            std::string v(vlen, '\0');
            for (size_t j = 0; j < vlen; j++) v[j] = static_cast<char>('!' + (rand() % 94));
            hashSetExpiry(o, fields[idx].c_str(), v.c_str(), mstime() + 100000 + rand());
        } else if (action < 90) {
            /* DELETE: 15% */
            size_t idx = static_cast<size_t>(rand()) % fields.size();
            hashDel(o, fields[idx].c_str());
            fields.erase(fields.begin() + static_cast<long>(idx));
        } else {
            /* DUP + verify: 10% */
            if (o->encoding == OBJ_ENCODING_HASHTABLE) {
                robj *copy = hashTypeDup(o);
                hashtable *ht_copy = static_cast<hashtable *>(objectGetVal(copy));
                ASSERT_TRACKED_CORRECT(ht_copy);
                decrRefCount(copy);
            }
        }

        if (!converted && o->encoding == OBJ_ENCODING_HASHTABLE) {
            converted = true;
        }

        if (o->encoding == OBJ_ENCODING_HASHTABLE) {
            hashtable *ht = static_cast<hashtable *>(objectGetVal(o));
            ASSERT_TRACKED_CORRECT(ht);
        }
    }

    ASSERT_TRUE(converted) << "Fuzzer never triggered listpack→hashtable conversion";

    /* Delete all remaining */
    if (o->encoding == OBJ_ENCODING_HASHTABLE) {
        hashtable *ht = static_cast<hashtable *>(objectGetVal(o));
        while (!fields.empty()) {
            hashDel(o, fields.back().c_str());
            fields.pop_back();
            ASSERT_TRACKED_CORRECT(ht);
        }
        ASSERT_EQ(hashtableTrackedDataBytes(ht), 0ul);
    }

    server.hash_max_listpack_entries = saved;
    server.hash_max_listpack_value = saved_val;
    decrRefCount(o);
}
