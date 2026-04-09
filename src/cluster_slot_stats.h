#include "server.h"
#include "cluster.h"
#include "cluster_legacy.h"

/* General use-cases. */
void clusterSlotStatReset(int slot);
void clusterSlotStatResetAll(void);
int clusterSlotStatsEnabled(int slot);

/* cpu-usec metric. */
void clusterSlotStatsAddCpuDuration(client *c, ustime_t duration);

/* network-bytes-in metric. */
void clusterSlotStatsAddNetworkBytesInForUserClient(client *c);

/* memory-data-bytes / memory-overhead-bytes metrics. */
#define SLOT_MEM_KEYS_STATIC 4
typedef struct slotMemKeys {
    sds buf[SLOT_MEM_KEYS_STATIC];
    sds *keys;
    int count;
} slotMemKeys;
void clusterSlotStatsSnapshotMemoryBefore(client *c, slotMemKeys *sk);
void clusterSlotStatsApplyMemoryAfter(client *c, slotMemKeys *sk);
void clusterSlotStatsFreeKeys(slotMemKeys *sk);
void clusterSlotStatsRecountMemory(void);

/* network-bytes-out metric. */
void clusterSlotStatsAddNetworkBytesOutForSlot(int slot, unsigned long long net_bytes_out);
void clusterSlotStatsAddNetworkBytesOutForUserClient(client *c);
void clusterSlotStatsIncrNetworkBytesOutForReplication(long long len);
void clusterSlotStatsDecrNetworkBytesOutForReplication(long long len);
void clusterSlotStatsAddNetworkBytesOutForShardedPubSubInternalPropagation(client *c, int slot);
