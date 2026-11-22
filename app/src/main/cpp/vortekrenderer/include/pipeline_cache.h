#ifndef VORTEK_PIPELINE_CACHE_H
#define VORTEK_PIPELINE_CACHE_H

#include <stdatomic.h>
#include "vortek.h"

/*
 * LURVETA Performance — persistent VkPipelineCache owned by the Vortek server.
 *
 * DXVK 2.x and wined3d create pipelines with VK_NULL_HANDLE as pipeline cache, so every launch
 * compiles every shader again in the Mali driver (shader stutter on each run). When — and only
 * when — the application passes no cache, Vortek substitutes this one. Its contents are saved to
 * APP_CACHE_DIR/vortek/ and loaded on the next launch.
 *
 * Safety: the file carries its own header (vendor, device, driver version, pipelineCacheUUID, size,
 * checksum); anything that does not match exactly is discarded and the cache starts empty, so the
 * driver never receives foreign or truncated data. Applications that use their own VkPipelineCache
 * are not affected. Can be disabled in the Vortek settings ("pipelineCache").
 */

typedef struct PersistentPipelineCache {
    VkDevice device;
    VkPipelineCache cache;
    char path[256];
    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t driverVersion;
    uint8_t uuid[VK_UUID_SIZE];
    atomic_uint createdSinceSave;
    atomic_flag saving;
    uint64_t lastSaveNanos;
    size_t loadedBytes;
} PersistentPipelineCache;

extern PersistentPipelineCache* PersistentPipelineCache_create(VkPhysicalDevice physicalDevice, VkDevice device);
/* Called by the pipeline creation threads after a successful vkCreate*Pipelines with this cache. */
extern void PersistentPipelineCache_onPipelinesCreated(PersistentPipelineCache* pipelineCache, uint32_t count);
extern bool PersistentPipelineCache_save(PersistentPipelineCache* pipelineCache);
/* Saves, then destroys the VkPipelineCache. The device must still be alive and idle for pipelines. */
extern void PersistentPipelineCache_destroy(PersistentPipelineCache* pipelineCache);

#endif
