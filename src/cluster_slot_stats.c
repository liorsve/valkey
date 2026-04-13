/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "cluster_slot_stats.h"

#define UNASSIGNED_SLOT 0

typedef enum {
    KEY_COUNT,
    CPU_USEC,
    NETWORK_BYTES_IN,
    NETWORK_BYTES_OUT,
    DATA_BYTES,
    OVERHEAD_BYTES,
    SLOT_STAT_COUNT,
    INVALID
} slotStatType;

/* -----------------------------------------------------------------------------
 * CLUSTER SLOT-STATS command
 * -------------------------------------------------------------------------- */

/* Struct used to temporarily hold slot statistics for sorting. */
typedef struct {
    int slot;
    uint64_t stat;
} slotStatForSort;

static int doesSlotBelongToMyShard(int slot) {
    clusterNode *myself = getMyClusterNode();
    clusterNode *primary = clusterNodeGetPrimary(myself);

    return clusterNodeCoversSlot(primary, slot);
}

static int markSlotsAssignedToMyShard(unsigned char *assigned_slots, int start_slot, int end_slot) {
    int assigned_slots_count = 0;
    for (int slot = start_slot; slot <= end_slot; slot++) {
        if (doesSlotBelongToMyShard(slot)) {
            assigned_slots[slot]++;
            assigned_slots_count++;
        }
    }
    return assigned_slots_count;
}

static uint64_t getSlotStat(int slot, slotStatType stat_type) {
    uint64_t slot_stat = 0;
    switch (stat_type) {
    case KEY_COUNT: slot_stat = countKeysInSlot(slot); break;
    case CPU_USEC: slot_stat = server.cluster->slot_stats[slot].cpu_usec; break;
    case NETWORK_BYTES_IN: slot_stat = server.cluster->slot_stats[slot].network_bytes_in; break;
    case NETWORK_BYTES_OUT: slot_stat = server.cluster->slot_stats[slot].network_bytes_out; break;
    case DATA_BYTES: slot_stat = server.cluster->slot_stats[slot].data_bytes; break;
    case OVERHEAD_BYTES: slot_stat = server.cluster->slot_stats[slot].overhead_bytes; break;
    case SLOT_STAT_COUNT:
    case INVALID: serverPanic("Invalid slot stat type %d was found.", stat_type);
    }
    return slot_stat;
}

/* Compare by stat in ascending order. If stat is the same, compare by slot in ascending order. */
static int slotStatForSortAscCmp(const void *a, const void *b) {
    slotStatForSort entry_a = *((slotStatForSort *)a);
    slotStatForSort entry_b = *((slotStatForSort *)b);
    if (entry_a.stat == entry_b.stat) {
        return entry_a.slot - entry_b.slot;
    }
    return entry_a.stat - entry_b.stat;
}

/* Compare by stat in descending order. If stat is the same, compare by slot in ascending order. */
static int slotStatForSortDescCmp(const void *a, const void *b) {
    slotStatForSort entry_a = *((slotStatForSort *)a);
    slotStatForSort entry_b = *((slotStatForSort *)b);
    if (entry_b.stat == entry_a.stat) {
        return entry_a.slot - entry_b.slot;
    }
    return entry_b.stat - entry_a.stat;
}

static void collectAndSortSlotStats(slotStatForSort slot_stats[], slotStatType order_by, int desc) {
    int i = 0;

    for (int slot = 0; slot < CLUSTER_SLOTS; slot++) {
        if (doesSlotBelongToMyShard(slot)) {
            slot_stats[i].slot = slot;
            slot_stats[i].stat = getSlotStat(slot, order_by);
            i++;
        }
    }
    qsort(slot_stats, i, sizeof(slotStatForSort), (desc) ? slotStatForSortDescCmp : slotStatForSortAscCmp);
}

static void addReplySlotStat(client *c, int slot) {
    addReplyArrayLen(c, 2); /* Array of size 2, where 0th index represents (int) slot,
                             * and 1st index represents (map) usage statistics. */
    addReplyLongLong(c, slot);
    addReplyMapLen(c, (server.cluster_slot_stats_enabled) ? SLOT_STAT_COUNT
                                                          : 1); /* Nested map representing slot usage statistics. */
    addReplyBulkCString(c, "key-count");
    addReplyLongLong(c, countKeysInSlot(slot));

    /* Any additional metrics aside from key-count come with a performance trade-off,
     * and are aggregated and returned based on its server config. */
    if (server.cluster_slot_stats_enabled) {
        addReplyBulkCString(c, "cpu-usec");
        addReplyLongLong(c, server.cluster->slot_stats[slot].cpu_usec);
        addReplyBulkCString(c, "network-bytes-in");
        addReplyLongLong(c, server.cluster->slot_stats[slot].network_bytes_in);
        addReplyBulkCString(c, "network-bytes-out");
        addReplyLongLong(c, server.cluster->slot_stats[slot].network_bytes_out);
        addReplyBulkCString(c, "memory-data-bytes");
        addReplyLongLong(c, server.cluster->slot_stats[slot].data_bytes);
        addReplyBulkCString(c, "memory-overhead-bytes");
        addReplyLongLong(c, server.cluster->slot_stats[slot].overhead_bytes);
    }
}

/* Adds reply for the SLOTSRANGE variant.
 * Response is ordered in ascending slot number. */
static void addReplySlotsRange(client *c, unsigned char *assigned_slots, int startslot, int endslot, int len) {
    addReplyArrayLen(c, len); /* Top level RESP reply format is defined as an array, due to ordering invariance. */

    for (int slot = startslot; slot <= endslot; slot++) {
        if (assigned_slots[slot]) addReplySlotStat(c, slot);
    }
}

static void addReplySortedSlotStats(client *c, slotStatForSort slot_stats[], long limit) {
    int num_slots_assigned = getMyShardSlotCount();
    int len = min(limit, num_slots_assigned);
    addReplyArrayLen(c, len); /* Top level RESP reply format is defined as an array, due to ordering invariance. */

    for (int i = 0; i < len; i++) {
        addReplySlotStat(c, slot_stats[i].slot);
    }
}

/* Accumulates egress bytes for the slot. */
void clusterSlotStatsAddNetworkBytesOutForSlot(int slot, unsigned long long net_bytes_out) {
    if (!clusterSlotStatsEnabled(slot)) return;

    serverAssert(slot >= 0 && slot < CLUSTER_SLOTS);
    server.cluster->slot_stats[slot].network_bytes_out += net_bytes_out;
}

/* Accumulates egress bytes upon sending RESP responses back to user clients. */
void clusterSlotStatsAddNetworkBytesOutForUserClient(client *c) {
    clusterSlotStatsAddNetworkBytesOutForSlot(c->slot, c->net_output_bytes_curr_cmd);
}

/* Accumulates egress bytes upon sending replication stream. This only applies for primary nodes. */
static void clusterSlotStatsUpdateNetworkBytesOutForReplication(long long len) {
    client *c = server.current_client;
    if (c == NULL || !clusterSlotStatsEnabled(c->slot)) return;

    /* We multiply the bytes len by the number of replicas to account for us broadcasting to multiple replicas at once. */
    len *= (long long)listLength(server.replicas);
    serverAssert(c->slot >= 0 && c->slot < CLUSTER_SLOTS);
    serverAssert(nodeIsPrimary(server.cluster->myself));
    /* We sometimes want to adjust the counter downwards (for example when we want to undo accounting for
     * SELECT commands that don't belong to any slot) so let's make sure we don't underflow the counter. */
    serverAssert(len >= 0 || server.cluster->slot_stats[c->slot].network_bytes_out >= (uint64_t)-len);
    server.cluster->slot_stats[c->slot].network_bytes_out += len;
}

/* Increment network bytes out for replication stream. This method will increment `len` value times the active replica
 * count. */
void clusterSlotStatsIncrNetworkBytesOutForReplication(long long len) {
    clusterSlotStatsUpdateNetworkBytesOutForReplication(len);
}

/* Decrement network bytes out for replication stream.
 * This is used to remove accounting of data which doesn't belong to any particular slots e.g. SELECT command.
 * This will decrement `len` value times the active replica count. */
void clusterSlotStatsDecrNetworkBytesOutForReplication(long long len) {
    clusterSlotStatsUpdateNetworkBytesOutForReplication(-len);
}

/* Upon SPUBLISH, two egress events are triggered.
 * 1) Internal propagation, for clients that are subscribed to the current node.
 * 2) External propagation, for other nodes within the same shard (could either be a primary or replica).
 *    This type is not aggregated, to stay consistent with server.stat_net_output_bytes aggregation.
 * This function covers the internal propagation component. */
void clusterSlotStatsAddNetworkBytesOutForShardedPubSubInternalPropagation(client *c, int slot) {
    if (!clusterSlotStatsEnabled(slot)) return;

    serverAssert(slot >= 0 && slot < CLUSTER_SLOTS);
    server.cluster->slot_stats[slot].network_bytes_out += c->net_output_bytes_curr_cmd;

    /* For sharded pubsub, the client's network bytes metrics must be reset here,
     * as resetClient() is not called until subscription ends. */
    c->net_output_bytes_curr_cmd = 0;
}

/* Adds reply for the ORDERBY variant.
 * Response is ordered based on the sort result. */
static void addReplyOrderBy(client *c, slotStatType order_by, long limit, int desc) {
    slotStatForSort slot_stats[CLUSTER_SLOTS];
    collectAndSortSlotStats(slot_stats, order_by, desc);
    addReplySortedSlotStats(c, slot_stats, limit);
}

/* Resets applicable slot statistics. */
void clusterSlotStatReset(int slot) {
    /* key-count is exempt, as it is queried separately through `countKeysInSlot()`. */
    memset(&server.cluster->slot_stats[slot], 0, sizeof(slotStat));
}

void clusterSlotStatResetAll(void) {
    memset(server.cluster->slot_stats, 0, sizeof(server.cluster->slot_stats));
}

/* For cpu-usec accumulation, nested commands within EXEC, EVAL, FCALL are skipped.
 * This is due to their unique callstack, where the c->duration for
 * EXEC, EVAL and FCALL already includes all of its nested commands.
 * Meaning, the accumulation of cpu-usec for these nested commands
 * would equate to repeating the same calculation twice.
 */
static int canAddCpuDuration(client *c) {
    return clusterSlotStatsEnabled(c->slot) &&
           (!server.execution_nesting ||         /* Either; */
            (server.execution_nesting &&         /* 1) Command should not be nested, or */
             c->realcmd->flags & CMD_BLOCKING)); /* 2) If command is nested, it must be due to unblocking. */
}

void clusterSlotStatsAddCpuDuration(client *c, ustime_t duration) {
    if (!canAddCpuDuration(c)) return;

    serverAssert(c->slot >= 0 && c->slot < CLUSTER_SLOTS);
    server.cluster->slot_stats[c->slot].cpu_usec += duration;
}

static int canAddNetworkBytesIn(client *c) {
    /* First, cluster mode must be enabled.
     * Second, command should target a specific slot.
     * Third, blocked client is not aggregated, to avoid duplicate aggregation upon unblocking.
     * Fourth, the server is not under a MULTI/EXEC transaction, to avoid duplicate aggregation of
     * EXEC's 14 bytes RESP upon nested call()'s afterCommand(). */
    return clusterSlotStatsEnabled(c->slot) && !(c->flag.blocked) && !server.in_exec;
}

/* Adds network ingress bytes of the current command in execution,
 * calculated earlier within networking.c layer.
 *
 * Note: Below function should only be called once c->slot is parsed.
 * Otherwise, the aggregation will be skipped due to canAddNetworkBytesIn() check failure.
 * */
void clusterSlotStatsAddNetworkBytesInForUserClient(client *c) {
    if (!canAddNetworkBytesIn(c)) return;

    if (c->cmd->proc == execCommand) {
        /* Accumulate its corresponding MULTI RESP; *1\r\n$5\r\nmulti\r\n */
        c->net_input_bytes_curr_cmd += 15;
    }

    server.cluster->slot_stats[c->slot].network_bytes_in += c->net_input_bytes_curr_cmd;
}

void clusterSlotStatsCommand(client *c) {
    if (!server.cluster_enabled) {
        addReplyError(c, "This instance has cluster support disabled");
        return;
    }

    /* Parse additional arguments. */
    if (c->argc == 5 && !strcasecmp(objectGetVal(c->argv[2]), "slotsrange")) {
        /* CLUSTER SLOT-STATS SLOTSRANGE start-slot end-slot */
        int startslot, endslot;
        if ((startslot = getSlotOrReply(c, c->argv[3])) == -1 ||
            (endslot = getSlotOrReply(c, c->argv[4])) == -1) {
            return;
        }
        if (startslot > endslot) {
            addReplyErrorFormat(c, "Start slot number %d is greater than end slot number %d", startslot, endslot);
            return;
        }
        /* Initialize slot assignment array. */
        unsigned char assigned_slots[CLUSTER_SLOTS] = {UNASSIGNED_SLOT};
        int assigned_slots_count = markSlotsAssignedToMyShard(assigned_slots, startslot, endslot);
        addReplySlotsRange(c, assigned_slots, startslot, endslot, assigned_slots_count);

    } else if (c->argc >= 4 && !strcasecmp(objectGetVal(c->argv[2]), "orderby")) {
        /* CLUSTER SLOT-STATS ORDERBY metric [LIMIT limit] [ASC | DESC] */
        int desc = 1;
        slotStatType order_by = INVALID;
        if (!strcasecmp(objectGetVal(c->argv[3]), "key-count")) {
            order_by = KEY_COUNT;
        } else if (!strcasecmp(objectGetVal(c->argv[3]), "cpu-usec") && server.cluster_slot_stats_enabled) {
            order_by = CPU_USEC;
        } else if (!strcasecmp(objectGetVal(c->argv[3]), "network-bytes-in") && server.cluster_slot_stats_enabled) {
            order_by = NETWORK_BYTES_IN;
        } else if (!strcasecmp(objectGetVal(c->argv[3]), "network-bytes-out") && server.cluster_slot_stats_enabled) {
            order_by = NETWORK_BYTES_OUT;
        } else if (!strcasecmp(objectGetVal(c->argv[3]), "memory-data-bytes") && server.cluster_slot_stats_enabled) {
            order_by = DATA_BYTES;
        } else if (!strcasecmp(objectGetVal(c->argv[3]), "memory-overhead-bytes") && server.cluster_slot_stats_enabled) {
            order_by = OVERHEAD_BYTES;
        } else {
            addReplyError(c, "Unrecognized sort metric for ORDERBY.");
            return;
        }
        int i = 4; /* Next argument index, following ORDERBY */
        int limit_counter = 0, asc_desc_counter = 0;
        long limit = CLUSTER_SLOTS;
        while (i < c->argc) {
            int moreargs = c->argc > i + 1;
            if (!strcasecmp(objectGetVal(c->argv[i]), "limit") && moreargs) {
                if (getRangeLongFromObjectOrReply(
                        c, c->argv[i + 1], 1, CLUSTER_SLOTS, &limit,
                        "Limit has to lie in between 1 and 16384 (maximum number of slots).") != C_OK) {
                    return;
                }
                i++;
                limit_counter++;
            } else if (!strcasecmp(objectGetVal(c->argv[i]), "asc")) {
                desc = 0;
                asc_desc_counter++;
            } else if (!strcasecmp(objectGetVal(c->argv[i]), "desc")) {
                desc = 1;
                asc_desc_counter++;
            } else {
                addReplyErrorObject(c, shared.syntaxerr);
                return;
            }
            if (limit_counter > 1 || asc_desc_counter > 1) {
                addReplyError(c, "Multiple filters of the same type are disallowed.");
                return;
            }
            i++;
        }
        addReplyOrderBy(c, order_by, limit, desc);

    } else {
        addReplySubcommandSyntaxError(c);
    }
}

int clusterSlotStatsEnabled(int slot) {
    return server.cluster_slot_stats_enabled && server.cluster_enabled && slot != -1;
}

/* --------------------------------------------------------------------------
 * Per-slot memory tracking via per-key size cache + signalModifiedKey.
 *
 * Each key has a cached {data_bytes, overhead_bytes} in a single hashtable
 * (db->key_mem_cache). signalModifiedKey is the single hook: it computes
 * the current objectLogicalSize, diffs against the cached value, updates
 * slot_stats, and refreshes the cache. For deleted keys, current=0 and the
 * cache provides the old size. For new keys, cache=0 and current provides
 * the new size.
 * -------------------------------------------------------------------------- */

/* Cache entry stored in db->key_mem_cache. */
typedef struct keySizeCacheEntry {
    sds key;
    size_t data_bytes;
    size_t overhead_bytes;
} keySizeCacheEntry;

static const void *keySizeCacheGetKey(const void *entry) {
    return ((const keySizeCacheEntry *)entry)->key;
}

static void keySizeCacheEntryDestructor(void *entry) {
    keySizeCacheEntry *e = entry;
    sdsfree(e->key);
    zfree(e);
}

hashtableType keySizeCacheHashtableType = {
    .entryGetKey = keySizeCacheGetKey,
    .hashFunction = sdsHashConfigurableSeed,
    .keyCompare = dictSdsKeyCompare,
    .entryDestructor = keySizeCacheEntryDestructor,
};

/* Called from signalModifiedKey after every key mutation.
 * Computes current size, diffs against cache, updates slot stats and cache. */
void clusterSlotStatsHandleKeyModified(serverDb *db, robj *key) {
    if (!db->key_mem_cache) return;
    sds keyname = objectGetVal(key);
    int slot = getKVStoreIndexForKey(keyname);
    if (!clusterSlotStatsEnabled(slot)) return;

    /* Look up current value (NULL if key was just deleted). */
    robj *val = dbFind(db, keyname);
    size_t cur_data = 0, cur_overhead = 0;
    if (val) {
        objectLogicalSize(val, &cur_data, &cur_overhead);
    }

    /* Look up cached size. */
    size_t old_data = 0, old_overhead = 0;
    void *existing = NULL;
    int found = hashtableFind(db->key_mem_cache, keyname, &existing);
    if (found) {
        keySizeCacheEntry *cached = existing;
        old_data = cached->data_bytes;
        old_overhead = cached->overhead_bytes;
    }

    /* Apply delta to slot stats. */
    int64_t data_delta = (int64_t)cur_data - (int64_t)old_data;
    int64_t overhead_delta = (int64_t)cur_overhead - (int64_t)old_overhead;
    if (data_delta != 0) server.cluster->slot_stats[slot].data_bytes += data_delta;
    if (overhead_delta != 0) server.cluster->slot_stats[slot].overhead_bytes += overhead_delta;

    /* Update or remove cache entry. */
    if (val) {
        if (found) {
            keySizeCacheEntry *cached = existing;
            cached->data_bytes = cur_data;
            cached->overhead_bytes = cur_overhead;
        } else {
            keySizeCacheEntry *e = zmalloc(sizeof(keySizeCacheEntry));
            e->key = sdsdup(keyname);
            e->data_bytes = cur_data;
            e->overhead_bytes = cur_overhead;
            hashtableAdd(db->key_mem_cache, e);
        }
    } else {
        /* Key was deleted — remove cache entry. */
        if (found) {
            hashtableDelete(db->key_mem_cache, keyname);
        }
    }
}

/* Called from call() after read commands on hashtable-encoded values.
 * Detects overhead changes from incremental rehashing during reads.
 * Compares current hashtableMemUsage against the cached overhead —
 * only refreshes the cache when they differ. O(1) per call. */
void clusterSlotStatsHandleRehashOverhead(client *c) {
    if (!c->db->key_mem_cache) return;

    sds keyname = objectGetVal(c->argv[1]);
    int slot = keyHashSlot(keyname, (int)sdslen(keyname));
    void *found = NULL;
    if (!kvstoreHashtableFind(c->db->keys, slot, keyname, &found)) return;
    robj *val = found;

    /* Only hashtable-encoded values have rehashing overhead drift. */
    if (!val) return;
    if (val->encoding != OBJ_ENCODING_HASHTABLE) return;
    if (val->type != OBJ_SET && val->type != OBJ_HASH) return;

    hashtable *ht = objectGetVal(val);
    size_t current_overhead = hashtableMemUsage(ht);
    if (val->type == OBJ_HASH) {
        vset *volatile_fields = hashtableMetadata(ht);
        if (vsetIsValid(volatile_fields)) current_overhead += vsetLogicalSize(volatile_fields);
    }

    /* Check if cached overhead differs from current. */
    void *cache_entry = NULL;
    if (!hashtableFind(c->db->key_mem_cache, keyname, &cache_entry)) return;
    keySizeCacheEntry *cached = cache_entry;
    if (cached->overhead_bytes == current_overhead) return;

    /* Overhead changed — update slot stats and cache. */
    int64_t delta = (int64_t)current_overhead - (int64_t)cached->overhead_bytes;
    server.cluster->slot_stats[slot].overhead_bytes += delta;
    cached->overhead_bytes = current_overhead;
}

/* Active-defrag callback for key_mem_cache entries.
 * Called via hashtableScanDefrag with HASHTABLE_SCAN_EMIT_REF. */
void clusterSlotStatsDefragKeySizeCache(void *privdata, void *entry_ref) {
    UNUSED(privdata);
    keySizeCacheEntry **ref = (keySizeCacheEntry **)entry_ref;
    keySizeCacheEntry *entry = *ref;

    /* Try to defrag the entry struct itself. */
    keySizeCacheEntry *newentry = activeDefragAlloc(entry);
    if (newentry) {
        entry = newentry;
        *ref = newentry;
    }

    /* Try to defrag the sds key. */
    sds newsds = activeDefragSds(entry->key);
    if (newsds) entry->key = newsds;
}

/* Called from signalFlushedDb to reset memory counters on FLUSHALL/FLUSHDB. */
void clusterSlotStatsResetMemoryOnFlush(void) {
    if (!server.cluster_enabled || !server.cluster_slot_stats_enabled) return;
    for (int slot = 0; slot < CLUSTER_SLOTS; slot++) {
        server.cluster->slot_stats[slot].data_bytes = 0;
        server.cluster->slot_stats[slot].overhead_bytes = 0;
    }
    /* The key_mem_cache hashtable is emptied by the DB flush itself
     * (emptyDbAsync recreates it). */
}

/* Called from dbAddRDBLoad to track keys loaded from RDB. */
void clusterSlotStatsTrackRDBLoad(serverDb *db, sds key, robj *val) {
    int slot = getKVStoreIndexForKey(key);
    if (!clusterSlotStatsEnabled(slot)) return;

    size_t data, overhead;
    objectLogicalSize(val, &data, &overhead);
    server.cluster->slot_stats[slot].data_bytes += (int64_t)data;
    server.cluster->slot_stats[slot].overhead_bytes += (int64_t)overhead;

    if (db->key_mem_cache) {
        keySizeCacheEntry *e = zmalloc(sizeof(keySizeCacheEntry));
        e->key = sdsdup(key);
        e->data_bytes = data;
        e->overhead_bytes = overhead;
        hashtableAdd(db->key_mem_cache, e);
    }
}
