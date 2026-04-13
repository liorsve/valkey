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
extern hashtableType keySizeCacheHashtableType;
void clusterSlotStatsHandleKeyModified(serverDb *db, robj *key);
void clusterSlotStatsHandleRehashOverhead(client *c);
void clusterSlotStatsResetMemoryOnFlush(void);
void clusterSlotStatsTrackRDBLoad(serverDb *db, sds key, robj *val);
void clusterSlotStatsDefragKeySizeCache(void *privdata, void *entry_ref);

/* network-bytes-out metric. */
void clusterSlotStatsAddNetworkBytesOutForSlot(int slot, unsigned long long net_bytes_out);
void clusterSlotStatsAddNetworkBytesOutForUserClient(client *c);
void clusterSlotStatsIncrNetworkBytesOutForReplication(long long len);
void clusterSlotStatsDecrNetworkBytesOutForReplication(long long len);
void clusterSlotStatsAddNetworkBytesOutForShardedPubSubInternalPropagation(client *c, int slot);
