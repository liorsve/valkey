/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Correctness tests for SET hashtable tracked_data_bytes.
 * Covers every mutation path: add (two-phase insert, hashtableAdd),
 * remove (hashtableDelete, hashtablePop, two-phase pop), conversion
 * (listpack→ht, intset→ht), dup, defrag (replaceReallocatedEntry),
 * empty/release, and a fuzzer for randomized coverage.
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
#include "hashtable.h"
#include "intset.h"
#include "sds.h"
#include "server.h"
#include "zmalloc.h"

robj *setTypePopRandom(robj *set);
}

class SetTrackingTest : public ::testing::Test {};

/* ── Ground-truth helper ────────────────────────────────────────────── */

/* Walk all entries and sum logical SDS sizes — the ground truth. */
static size_t computeExpectedDataBytes(hashtable *ht) {
    size_t total = 0;
    hashtableIterator iter;
    hashtableInitIterator(&iter, ht, 0);
    void *entry;
    while (hashtableNext(&iter, &entry)) {
        sds s = static_cast<sds>(entry);
        total += sdsHdrSize(sdsType(s)) + sdslen(s) + 1;
    }
    hashtableCleanupIterator(&iter);
    return total;
}

#define ASSERT_TRACKED_CORRECT(ht)                                 \
    do {                                                           \
        size_t _expected = computeExpectedDataBytes(ht);           \
        ASSERT_EQ(hashtableTrackedDataBytes(ht), _expected)        \
            << "tracked_data_bytes mismatch";                      \
    } while (0)

/* ── Helpers ────────────────────────────────────────────────────────── */

/* Add a C-string to a SET robj. Handles ownership correctly. */
static void setAdd(robj *set, const char *str) {
    sds s = sdsnew(str);
    if (!setTypeAdd(set, s)) sdsfree(s);
}

/* Remove a C-string from a SET robj. */
static int setRemove(robj *set, const char *str) {
    sds s = sdsnew(str);
    int removed = setTypeRemove(set, s);
    sdsfree(s);
    return removed;
}

/* Make a string of a given length filled with ch. */
static std::string makeString(size_t len, char ch = 'x') {
    return std::string(len, ch);
}

/* ── 1. setTypeAddAux: two-phase insert (normal hashtable add path) ── */
TEST_F(SetTrackingTest, AddViaTwoPhaseInsert) {
    robj *set = createSetObject();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(set));

    for (int i = 0; i < 100; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "element_%d", i);
        setAdd(set, buf);
        ASSERT_TRACKED_CORRECT(ht);
    }

    decrRefCount(set);
}

/* ── 2. hashtableAdd: direct API (used during conversion) ───────────── */
TEST_F(SetTrackingTest, DirectHashtableAdd) {
    hashtable *ht = hashtableCreate(&setHashtableType);

    for (int i = 0; i < 50; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "direct_%d", i);
        sds s = sdsnew(buf);
        ASSERT_TRUE(hashtableAdd(ht, s));
        ASSERT_TRACKED_CORRECT(ht);
    }

    hashtableRelease(ht);
}

/* ── 3. setTypeRemoveAux → hashtableDelete ──────────────────────────── */
TEST_F(SetTrackingTest, RemoveViaHashtableDelete) {
    robj *set = createSetObject();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(set));

    for (int i = 0; i < 50; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "elem_%d", i);
        setAdd(set, buf);
    }
    ASSERT_TRACKED_CORRECT(ht);

    /* Remove each entry one by one */
    for (int i = 0; i < 50; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "elem_%d", i);
        ASSERT_EQ(setRemove(set, buf), 1);
        ASSERT_TRACKED_CORRECT(ht);
    }
    ASSERT_EQ(hashtableTrackedDataBytes(ht), 0ul);

    decrRefCount(set);
}

/* ── 4. hashtablePop (remove without freeing) ───────────────────────── */
TEST_F(SetTrackingTest, HashtablePop) {
    hashtable *ht = hashtableCreate(&setHashtableType);

    sds a = sdsnew("alpha");
    sds b = sdsnew("beta");
    sds c = sdsnew("gamma");
    hashtableAdd(ht, a);
    hashtableAdd(ht, b);
    hashtableAdd(ht, c);
    ASSERT_TRACKED_CORRECT(ht);

    void *popped = nullptr;
    ASSERT_TRUE(hashtablePop(ht, b, &popped));
    ASSERT_TRACKED_CORRECT(ht);
    sdsfree(static_cast<sds>(popped));

    ASSERT_TRUE(hashtablePop(ht, a, &popped));
    ASSERT_TRACKED_CORRECT(ht);
    sdsfree(static_cast<sds>(popped));

    ASSERT_TRUE(hashtablePop(ht, c, &popped));
    ASSERT_TRACKED_CORRECT(ht);
    ASSERT_EQ(hashtableTrackedDataBytes(ht), 0ul);
    sdsfree(static_cast<sds>(popped));

    hashtableRelease(ht);
}

/* ── 5. Two-phase pop (FindRef + Delete) ────────────────────────────── */
TEST_F(SetTrackingTest, TwoPhasePopDelete) {
    hashtable *ht = hashtableCreate(&setHashtableType);

    sds s1 = sdsnew("one");
    sds s2 = sdsnew("two");
    sds s3 = sdsnew("three");
    hashtableAdd(ht, s1);
    hashtableAdd(ht, s2);
    hashtableAdd(ht, s3);
    ASSERT_TRACKED_CORRECT(ht);

    /* Two-phase pop "two" */
    hashtablePosition pos;
    void **ref = hashtableTwoPhasePopFindRef(ht, s2, &pos);
    ASSERT_NE(ref, nullptr);
    sds popped = static_cast<sds>(*ref);
    hashtableTwoPhasePopDelete(ht, &pos);
    ASSERT_TRACKED_CORRECT(ht);
    sdsfree(popped);

    hashtableRelease(ht);
}

/* ── 6. Two-phase insert (FindPositionForInsert + InsertAtPosition) ── */
TEST_F(SetTrackingTest, TwoPhaseInsert) {
    hashtable *ht = hashtableCreate(&setHashtableType);

    for (int i = 0; i < 30; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "twophase_%d", i);
        sds s = sdsnew(buf);
        hashtablePosition pos;
        void *existing = nullptr;
        ASSERT_TRUE(hashtableFindPositionForInsert(ht, s, &pos, &existing));
        hashtableInsertAtPosition(ht, s, &pos);
        ASSERT_TRACKED_CORRECT(ht);
    }

    hashtableRelease(ht);
}

/* ── 7. Duplicate add does not double-count ─────────────────────────── */
TEST_F(SetTrackingTest, DuplicateAdd) {
    robj *set = createSetObject();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(set));

    setAdd(set, "hello");
    size_t after_first = hashtableTrackedDataBytes(ht);
    ASSERT_GT(after_first, 0ul);

    /* Adding the same element again should not change tracked bytes */
    setAdd(set, "hello");
    ASSERT_EQ(hashtableTrackedDataBytes(ht), after_first);
    ASSERT_TRACKED_CORRECT(ht);

    decrRefCount(set);
}

/* ── 8. Remove nonexistent key does not affect tracking ─────────────── */
TEST_F(SetTrackingTest, RemoveNonexistent) {
    robj *set = createSetObject();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(set));

    setAdd(set, "a");
    setAdd(set, "b");
    size_t before = hashtableTrackedDataBytes(ht);

    ASSERT_EQ(setRemove(set, "nonexistent"), 0);
    ASSERT_EQ(hashtableTrackedDataBytes(ht), before);
    ASSERT_TRACKED_CORRECT(ht);

    decrRefCount(set);
}

/* ── 9. Various SDS sizes (TYPE_5, TYPE_8, TYPE_16) ─────────────────── */
TEST_F(SetTrackingTest, VariousStringSizes) {
    robj *set = createSetObject();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(set));

    /* SDS_TYPE_5: strings up to 31 bytes */
    setAdd(set, "x");
    setAdd(set, "short_string_value");
    ASSERT_TRACKED_CORRECT(ht);

    /* SDS_TYPE_8: strings 32..255 bytes */
    std::string med = makeString(100, 'M');
    setAdd(set, med.c_str());
    ASSERT_TRACKED_CORRECT(ht);

    std::string med2 = makeString(255, 'N');
    setAdd(set, med2.c_str());
    ASSERT_TRACKED_CORRECT(ht);

    /* SDS_TYPE_16: larger string */
    std::string big = makeString(1024, 'B');
    setAdd(set, big.c_str());
    ASSERT_TRACKED_CORRECT(ht);

    decrRefCount(set);
}

/* ── 10. Conversion: listpack → hashtable ────────────────────────────── */
TEST_F(SetTrackingTest, ConvertListpackToHashtable) {
    size_t saved_lp = server.set_max_listpack_entries;
    size_t saved_lp_val = server.set_max_listpack_value;
    server.set_max_listpack_entries = 128;
    server.set_max_listpack_value = 64;

    robj *set = createSetListpackObject();

    /* Add entries while still in listpack encoding */
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "lp_elem_%d", i);
        sds s = sdsnew(buf);
        setTypeAdd(set, s);
    }
    ASSERT_EQ(set->encoding, static_cast<unsigned int>(OBJ_ENCODING_LISTPACK));

    /* Force conversion to hashtable */
    setTypeConvert(set, OBJ_ENCODING_HASHTABLE);
    ASSERT_EQ(set->encoding, static_cast<unsigned int>(OBJ_ENCODING_HASHTABLE));

    hashtable *ht = static_cast<hashtable *>(objectGetVal(set));
    ASSERT_TRACKED_CORRECT(ht);
    ASSERT_GT(hashtableTrackedDataBytes(ht), 0ul);

    server.set_max_listpack_entries = saved_lp;
    server.set_max_listpack_value = saved_lp_val;
    decrRefCount(set);
}

/* ── 11. Conversion: intset → hashtable (non-integer triggers it) ──── */
TEST_F(SetTrackingTest, ConvertIntsetToHashtable) {
    size_t saved_intset = server.set_max_intset_entries;
    server.set_max_intset_entries = 512;

    robj *set = createIntsetObject();

    /* Add integers to keep it as intset */
    for (int i = 0; i < 10; i++) {
        sds s = sdsfromlonglong(i);
        setTypeAdd(set, s);
    }
    ASSERT_EQ(set->encoding, static_cast<unsigned int>(OBJ_ENCODING_INTSET));

    /* Force conversion to hashtable by adding a non-integer */
    server.set_max_listpack_entries = 0; /* skip listpack */
    sds non_int = sdsnew("not_an_integer");
    setTypeAdd(set, non_int);
    ASSERT_EQ(set->encoding, static_cast<unsigned int>(OBJ_ENCODING_HASHTABLE));

    hashtable *ht = static_cast<hashtable *>(objectGetVal(set));
    ASSERT_TRACKED_CORRECT(ht);

    /* Restore defaults */
    server.set_max_listpack_entries = 128;
    server.set_max_intset_entries = saved_intset;

    decrRefCount(set);
}

/* ── 12. Conversion: listpack overflow to hashtable via setTypeAddAux ── */
TEST_F(SetTrackingTest, ListpackOverflowToHashtable) {
    server.set_max_listpack_entries = 5;
    robj *set = createSetListpackObject();

    /* Fill listpack to limit, then overflow */
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "overflow_%d", i);
        sds s = sdsnew(buf);
        setTypeAdd(set, s);
    }

    /* Should have converted to hashtable */
    ASSERT_EQ(set->encoding, static_cast<unsigned int>(OBJ_ENCODING_HASHTABLE));
    hashtable *ht = static_cast<hashtable *>(objectGetVal(set));
    ASSERT_TRACKED_CORRECT(ht);

    server.set_max_listpack_entries = 128;
    decrRefCount(set);
}

/* ── 13. setTypeDup (COPY command path) ─────────────────────────────── */
TEST_F(SetTrackingTest, SetTypeDup) {
    robj *set = createSetObject();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(set));

    for (int i = 0; i < 50; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "dup_test_%d", i);
        setAdd(set, buf);
    }
    ASSERT_TRACKED_CORRECT(ht);

    /* Duplicate the set */
    robj *copy = setTypeDup(set);
    ASSERT_EQ(copy->encoding, static_cast<unsigned int>(OBJ_ENCODING_HASHTABLE));
    hashtable *ht_copy = static_cast<hashtable *>(objectGetVal(copy));
    ASSERT_TRACKED_CORRECT(ht_copy);

    /* Both should have the same tracked bytes */
    ASSERT_EQ(hashtableTrackedDataBytes(ht), hashtableTrackedDataBytes(ht_copy));

    decrRefCount(set);
    decrRefCount(copy);
}

/* ── 14. hashtableReplaceReallocatedEntry (defrag path) ─────────────── */
TEST_F(SetTrackingTest, ReplaceReallocatedEntry) {
    hashtable *ht = hashtableCreate(&setHashtableType);

    sds orig = sdsnew("defrag_target");
    hashtableAdd(ht, orig);
    size_t before = hashtableTrackedDataBytes(ht);
    ASSERT_TRACKED_CORRECT(ht);

    /* Simulate defrag: realloc the sds to a new address */
    sds copy = sdsdup(orig);
    bool replaced = hashtableReplaceReallocatedEntry(ht, orig, copy);
    ASSERT_TRUE(replaced);

    /* Tracked bytes should be unchanged */
    ASSERT_EQ(hashtableTrackedDataBytes(ht), before);
    ASSERT_TRACKED_CORRECT(ht);

    /* Free the old (now dangling) pointer — in real defrag, the allocator
     * handles this, but here we manually free the original. */
    sdsfree(orig);

    hashtableRelease(ht);
}

/* ── 15. hashtableEmpty resets tracking to zero ─────────────────────── */
TEST_F(SetTrackingTest, HashtableEmpty) {
    hashtable *ht = hashtableCreate(&setHashtableType);

    for (int i = 0; i < 100; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "empty_%d", i);
        hashtableAdd(ht, sdsnew(buf));
    }
    ASSERT_TRACKED_CORRECT(ht);

    hashtableEmpty(ht, nullptr);
    ASSERT_EQ(hashtableTrackedDataBytes(ht), 0ul);

    hashtableRelease(ht);
}

/* ── 16. Large interleaved adds and removes ─────────────────────────── */
TEST_F(SetTrackingTest, InterleavedAddRemove) {
    robj *set = createSetObject();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(set));

    /* Add 500 elements */
    for (int i = 0; i < 500; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key_%04d", i);
        setAdd(set, buf);
    }
    ASSERT_TRACKED_CORRECT(ht);

    /* Remove every other element */
    for (int i = 0; i < 500; i += 2) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key_%04d", i);
        setRemove(set, buf);
    }
    ASSERT_TRACKED_CORRECT(ht);

    /* Add new elements */
    for (int i = 500; i < 700; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key_%04d", i);
        setAdd(set, buf);
    }
    ASSERT_TRACKED_CORRECT(ht);

    decrRefCount(set);
}

/* ── 17. setTypePopRandom (SPOP path) ───────────────────────────────── */
TEST_F(SetTrackingTest, PopRandom) {
    robj *set = createSetObject();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(set));

    for (int i = 0; i < 20; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "pop_%d", i);
        setAdd(set, buf);
    }
    ASSERT_TRACKED_CORRECT(ht);

    /* Pop 10 random elements */
    for (int i = 0; i < 10; i++) {
        robj *ele = setTypePopRandom(set);
        ASSERT_NE(ele, nullptr);
        decrRefCount(ele);
        ASSERT_TRACKED_CORRECT(ht);
    }

    /* Pop remaining */
    while (setTypeSize(set) > 0) {
        robj *ele = setTypePopRandom(set);
        decrRefCount(ele);
    }
    ASSERT_EQ(hashtableTrackedDataBytes(ht), 0ul);

    decrRefCount(set);
}

/* ── 18. Delete all via safe iterator (server-side cleanup pattern) ── */
TEST_F(SetTrackingTest, DeleteViaSafeIterator) {
    hashtable *ht = hashtableCreate(&setHashtableType);

    for (int i = 0; i < 200; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "iter_del_%d", i);
        hashtableAdd(ht, sdsnew(buf));
    }
    ASSERT_TRACKED_CORRECT(ht);

    /* Delete all entries using a safe iterator */
    hashtableIterator iter;
    hashtableInitIterator(&iter, ht, HASHTABLE_ITER_SAFE);
    void *entry;
    while (hashtableNext(&iter, &entry)) {
        sds key = sdsdup(static_cast<sds>(entry));
        hashtableDelete(ht, key);
        sdsfree(key);
    }
    hashtableCleanupIterator(&iter);

    ASSERT_EQ(hashtableTrackedDataBytes(ht), 0ul);

    hashtableRelease(ht);
}

/* ── 19. Stress test: add 1000, remove half, add 500 more ──────────── */
TEST_F(SetTrackingTest, StressTest) {
    robj *set = createSetObject();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(set));

    /* Phase 1: add 1000 elements of varying sizes */
    for (int i = 0; i < 1000; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "stress_key_%06d", i);
        setAdd(set, buf);
    }
    ASSERT_TRACKED_CORRECT(ht);

    /* Phase 2: remove 500 */
    for (int i = 0; i < 1000; i += 2) {
        char buf[64];
        snprintf(buf, sizeof(buf), "stress_key_%06d", i);
        setRemove(set, buf);
    }
    ASSERT_TRACKED_CORRECT(ht);

    /* Phase 3: add 500 new with different sizes */
    for (int i = 1000; i < 1500; i++) {
        size_t len = 10 + (i % 200);
        std::string val(len, 'a' + (i % 26));
        setAdd(set, val.c_str());
    }
    ASSERT_TRACKED_CORRECT(ht);

    /* Phase 4: remove everything */
    while (setTypeSize(set) > 0) {
        robj *ele = setTypePopRandom(set);
        decrRefCount(ele);
    }
    ASSERT_EQ(hashtableTrackedDataBytes(ht), 0ul);

    decrRefCount(set);
}

/* ── 20. Fuzzer: comprehensive random operations across all paths ──── */
/*
 * Covers:
 *  - add (new key, duplicate key, varied string lengths incl. TYPE_5/8/16)
 *  - remove (existing key, non-existent key)
 *  - pop random (SPOP path)
 *  - encoding conversion (starts as listpack, triggers conversion to ht)
 *  - dup (COPY path — creates independent copy mid-run)
 *  - checks tracking after EVERY operation
 */
TEST_F(SetTrackingTest, Fuzzer) {
    /* Start as listpack so we also exercise the conversion path */
    server.set_max_listpack_entries = 32;
    robj *set = createSetListpackObject();

    unsigned seed = static_cast<unsigned>(time(nullptr)) ^ static_cast<unsigned>(getpid());
    srand(seed);
    printf("  Fuzzer seed: %u\n", seed);
    std::vector<std::string> known_keys;
    bool converted = false;

    const int NUM_OPS = 5000;

    for (int op = 0; op < NUM_OPS; op++) {
        int action = rand() % 100;

        if (action < 50 || known_keys.empty()) {
            /* ADD new key: 50% — varied sizes to hit SDS TYPE_5/8/16 */
            size_t len;
            int size_class = rand() % 10;
            if (size_class < 4)
                len = 1 + (static_cast<size_t>(rand()) % 30); /* TYPE_5 (≤31) */
            else if (size_class < 8)
                len = 32 + (static_cast<size_t>(rand()) % 224); /* TYPE_8 (32..255) */
            else
                len = 256 + (static_cast<size_t>(rand()) % 768); /* TYPE_16 (256..1023) */

            std::string key(len, '\0');
            for (size_t j = 0; j < len; j++) {
                key[j] = static_cast<char>('!' + (rand() % 94)); /* printable ASCII */
            }
            setAdd(set, key.c_str());
            known_keys.push_back(key);
        } else if (action < 60) {
            /* ADD duplicate: 10% — should not change tracked bytes */
            size_t idx = static_cast<size_t>(rand()) % known_keys.size();
            setAdd(set, known_keys[idx].c_str());
        } else if (action < 65) {
            /* REMOVE non-existent: 5% — should not change tracked bytes */
            std::string fake(20, '\0');
            for (size_t j = 0; j < 20; j++) {
                fake[j] = static_cast<char>('A' + (rand() % 26));
            }
            fake = "NONEXIST_" + fake;
            setRemove(set, fake.c_str());
        } else if (action < 85) {
            /* REMOVE existing: 20% */
            size_t idx = static_cast<size_t>(rand()) % known_keys.size();
            setRemove(set, known_keys[idx].c_str());
            known_keys.erase(known_keys.begin() + static_cast<long>(idx));
        } else if (action < 95) {
            /* POP random: 10% (SPOP path) */
            if (set->encoding == OBJ_ENCODING_HASHTABLE && setTypeSize(set) > 0) {
                robj *ele = setTypePopRandom(set);
                sds popped_sds = static_cast<sds>(objectGetVal(ele));
                std::string popped_str(popped_sds, sdslen(popped_sds));
                auto it = std::find(known_keys.begin(), known_keys.end(), popped_str);
                if (it != known_keys.end()) known_keys.erase(it);
                decrRefCount(ele);
            }
        } else {
            /* DUP + verify: 5% (COPY path) */
            if (set->encoding == OBJ_ENCODING_HASHTABLE) {
                robj *copy = setTypeDup(set);
                hashtable *ht_copy = static_cast<hashtable *>(objectGetVal(copy));
                ASSERT_TRACKED_CORRECT(ht_copy);
                decrRefCount(copy);
            }
        }

        /* Track conversion event */
        if (!converted && set->encoding == OBJ_ENCODING_HASHTABLE) {
            converted = true;
        }

        /* Check tracking after EVERY operation (only once in ht encoding) */
        if (set->encoding == OBJ_ENCODING_HASHTABLE) {
            hashtable *ht = static_cast<hashtable *>(objectGetVal(set));
            ASSERT_TRACKED_CORRECT(ht);
        }
    }

    /* Must have converted at some point */
    ASSERT_TRUE(converted) << "Fuzzer never triggered listpack→hashtable conversion";

    /* Drain everything and verify zero */
    while (setTypeSize(set) > 0) {
        robj *ele = setTypePopRandom(set);
        decrRefCount(ele);
    }
    if (set->encoding == OBJ_ENCODING_HASHTABLE) {
        hashtable *ht = static_cast<hashtable *>(objectGetVal(set));
        ASSERT_EQ(hashtableTrackedDataBytes(ht), 0ul);
    }

    server.set_max_listpack_entries = 128;
    decrRefCount(set);
}

/* ── 20b. Fuzzer starting from intset (integer-heavy workload) ──────── */
TEST_F(SetTrackingTest, FuzzerFromIntset) {
    server.set_max_listpack_entries = 0; /* force intset→hashtable (skip listpack) */
    robj *set = createIntsetObject();

    unsigned seed = static_cast<unsigned>(time(nullptr)) ^ static_cast<unsigned>(getpid()) ^ 0xDEAD;
    srand(seed);
    printf("  Fuzzer seed: %u\n", seed);
    std::vector<std::string> known_keys;
    bool converted = false;

    const int NUM_OPS = 3000;

    for (int op = 0; op < NUM_OPS; op++) {
        int action = rand() % 100;

        if (action < 55 || known_keys.empty()) {
            /* ADD: mix of integers and strings to trigger conversion */
            std::string key;
            if (!converted && rand() % 3 != 0) {
                /* Add integer while still intset */
                key = std::to_string(rand() % 100000);
            } else {
                /* Add string to force conversion or after conversion */
                size_t len = 1 + (static_cast<size_t>(rand()) % 200);
                key.resize(len);
                for (size_t j = 0; j < len; j++) {
                    key[j] = static_cast<char>('a' + (rand() % 26));
                }
            }
            setAdd(set, key.c_str());
            known_keys.push_back(key);
        } else if (action < 80) {
            /* REMOVE existing */
            size_t idx = static_cast<size_t>(rand()) % known_keys.size();
            setRemove(set, known_keys[idx].c_str());
            known_keys.erase(known_keys.begin() + static_cast<long>(idx));
        } else {
            /* POP random */
            if (set->encoding == OBJ_ENCODING_HASHTABLE && setTypeSize(set) > 0) {
                robj *ele = setTypePopRandom(set);
                sds popped_sds = static_cast<sds>(objectGetVal(ele));
                std::string popped_str(popped_sds, sdslen(popped_sds));
                auto it = std::find(known_keys.begin(), known_keys.end(), popped_str);
                if (it != known_keys.end()) known_keys.erase(it);
                decrRefCount(ele);
            }
        }

        if (!converted && set->encoding == OBJ_ENCODING_HASHTABLE) {
            converted = true;
        }

        if (set->encoding == OBJ_ENCODING_HASHTABLE) {
            hashtable *ht = static_cast<hashtable *>(objectGetVal(set));
            ASSERT_TRACKED_CORRECT(ht);
        }
    }

    ASSERT_TRUE(converted) << "Fuzzer never triggered intset→hashtable conversion";

    while (setTypeSize(set) > 0) {
        robj *ele = setTypePopRandom(set);
        decrRefCount(ele);
    }
    if (set->encoding == OBJ_ENCODING_HASHTABLE) {
        hashtable *ht = static_cast<hashtable *>(objectGetVal(set));
        ASSERT_EQ(hashtableTrackedDataBytes(ht), 0ul);
    }

    server.set_max_listpack_entries = 128;
    decrRefCount(set);
}

/* ── 21. Empty set: zero tracked bytes ──────────────────────────────── */
TEST_F(SetTrackingTest, EmptySetZeroBytes) {
    robj *set = createSetObject();
    hashtable *ht = static_cast<hashtable *>(objectGetVal(set));

    ASSERT_EQ(hashtableTrackedDataBytes(ht), 0ul);
    ASSERT_TRACKED_CORRECT(ht);

    decrRefCount(set);
}

