#include <sys/stat.h>
#include <time.h>

#include "pipeline_cache.h"
#include "vulkan_helper.h"
#include "string_utils.h"

#ifndef PIPELINE_CACHE_DIR
#define PIPELINE_CACHE_DIR APP_CACHE_DIR "/vortek"
#endif
#define PIPELINE_CACHE_MAGIC 0x4350564Cu /* "LVPC" */
#define PIPELINE_CACHE_FILE_VERSION 1
#define PIPELINE_CACHE_MAX_BYTES (128u << 20)
#define SAVE_MIN_NEW_PIPELINES 32
#define SAVE_MIN_INTERVAL_NS 20000000000ull

typedef struct PipelineCacheFileHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t driverVersion;
    uint8_t uuid[VK_UUID_SIZE];
    uint32_t reserved;
    uint64_t dataSize;
    uint64_t dataHash;
} PipelineCacheFileHeader;

static uint64_t nowNanos() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* Returns the cache data if the file belongs to this exact device + driver and is intact. */
static void* loadCacheFile(PersistentPipelineCache* pipelineCache, size_t* outSize) {
    *outSize = 0;
    FILE* file = fopen(pipelineCache->path, "rb");
    if (!file) return NULL;

    PipelineCacheFileHeader header = {0};
    void* data = NULL;
    const char* reason = NULL;

    if (fread(&header, sizeof(header), 1, file) != 1) reason = "short header";
    else if (header.magic != PIPELINE_CACHE_MAGIC || header.version != PIPELINE_CACHE_FILE_VERSION) reason = "unknown format";
    else if (header.vendorID != pipelineCache->vendorID || header.deviceID != pipelineCache->deviceID ||
             header.driverVersion != pipelineCache->driverVersion || memcmp(header.uuid, pipelineCache->uuid, VK_UUID_SIZE) != 0) reason = "other device or driver";
    else if (header.dataSize == 0 || header.dataSize > PIPELINE_CACHE_MAX_BYTES) reason = "invalid size";
    else {
        data = malloc(header.dataSize);
        if (fread(data, 1, header.dataSize, file) != header.dataSize) reason = "truncated";
        else if (murmurHash64(data, header.dataSize, PIPELINE_CACHE_MAGIC) != header.dataHash) reason = "checksum mismatch";
        else if (header.dataSize < 16 + VK_UUID_SIZE || ((uint32_t*)data)[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
                 memcmp((uint8_t*)data + 16, pipelineCache->uuid, VK_UUID_SIZE) != 0) reason = "driver header mismatch";
    }
    fclose(file);

    if (reason) {
        println("vortek: pipeline cache %s ignored (%s)", pipelineCache->path, reason);
        MEMFREE(data);
        unlink(pipelineCache->path);
        return NULL;
    }

    *outSize = header.dataSize;
    return data;
}

PersistentPipelineCache* PersistentPipelineCache_create(VkPhysicalDevice physicalDevice, VkDevice device) {
    VkPhysicalDeviceProperties properties = {0};
    vulkanWrapper.vkGetPhysicalDeviceProperties(physicalDevice, &properties);

    PersistentPipelineCache* pipelineCache = calloc(1, sizeof(PersistentPipelineCache));
    pipelineCache->device = device;
    pipelineCache->vendorID = properties.vendorID;
    pipelineCache->deviceID = properties.deviceID;
    pipelineCache->driverVersion = properties.driverVersion;
    memcpy(pipelineCache->uuid, properties.pipelineCacheUUID, VK_UUID_SIZE);
    atomic_flag_clear(&pipelineCache->saving);
    snprintf(pipelineCache->path, sizeof(pipelineCache->path), PIPELINE_CACHE_DIR "/pipeline-%x-%x.bin", properties.vendorID, properties.deviceID);

    size_t initialSize = 0;
    void* initialData = loadCacheFile(pipelineCache, &initialSize);

    VkPipelineCacheCreateInfo createInfo = {0};
    createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    createInfo.initialDataSize = initialSize;
    createInfo.pInitialData = initialData;

    VkResult result = vulkanWrapper.vkCreatePipelineCache(device, &createInfo, NULL, &pipelineCache->cache);
    if (result != VK_SUCCESS && initialData) {
        // The driver rejected the stored data: start from an empty cache.
        createInfo.initialDataSize = 0;
        createInfo.pInitialData = NULL;
        result = vulkanWrapper.vkCreatePipelineCache(device, &createInfo, NULL, &pipelineCache->cache);
        initialSize = 0;
    }
    MEMFREE(initialData);

    if (result != VK_SUCCESS) {
        println("vortek: persistent pipeline cache unavailable (result %d)", result);
        MEMFREE(pipelineCache);
        return NULL;
    }

    pipelineCache->loadedBytes = initialSize;
    pipelineCache->lastSaveNanos = nowNanos();
    println("vortek: persistent pipeline cache %s (%zu KB loaded)", pipelineCache->path, initialSize >> 10);
    return pipelineCache;
}

bool PersistentPipelineCache_save(PersistentPipelineCache* pipelineCache) {
    if (!pipelineCache || atomic_flag_test_and_set(&pipelineCache->saving)) return false;

    bool saved = false;
    size_t size = 0;
    void* data = NULL;
    VkResult result = vulkanWrapper.vkGetPipelineCacheData(pipelineCache->device, pipelineCache->cache, &size, NULL);
    if (result == VK_SUCCESS && size > 0 && size <= PIPELINE_CACHE_MAX_BYTES) {
        data = malloc(size);
        result = vulkanWrapper.vkGetPipelineCacheData(pipelineCache->device, pipelineCache->cache, &size, data);
    }

    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && data && size > 0) {
        PipelineCacheFileHeader header = {0};
        header.magic = PIPELINE_CACHE_MAGIC;
        header.version = PIPELINE_CACHE_FILE_VERSION;
        header.vendorID = pipelineCache->vendorID;
        header.deviceID = pipelineCache->deviceID;
        header.driverVersion = pipelineCache->driverVersion;
        memcpy(header.uuid, pipelineCache->uuid, VK_UUID_SIZE);
        header.dataSize = size;
        header.dataHash = murmurHash64(data, size, PIPELINE_CACHE_MAGIC);

        char tmpPath[sizeof(pipelineCache->path) + 8];
        snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", pipelineCache->path);
        mkdir(PIPELINE_CACHE_DIR, 0771);
        FILE* file = fopen(tmpPath, "wb");
        if (file) {
            saved = fwrite(&header, sizeof(header), 1, file) == 1 && fwrite(data, 1, size, file) == size;
            saved = (fclose(file) == 0) && saved;
            if (saved) saved = rename(tmpPath, pipelineCache->path) == 0;
            else unlink(tmpPath);
        }
    }

    MEMFREE(data);
    if (saved) {
        pipelineCache->lastSaveNanos = nowNanos();
        atomic_store(&pipelineCache->createdSinceSave, 0);
    }
    atomic_flag_clear(&pipelineCache->saving);
    return saved;
}

void PersistentPipelineCache_onPipelinesCreated(PersistentPipelineCache* pipelineCache, uint32_t count) {
    unsigned created = atomic_fetch_add(&pipelineCache->createdSinceSave, count) + count;
    if (created >= SAVE_MIN_NEW_PIPELINES && nowNanos() - pipelineCache->lastSaveNanos >= SAVE_MIN_INTERVAL_NS) {
        PersistentPipelineCache_save(pipelineCache);
    }
}

void PersistentPipelineCache_destroy(PersistentPipelineCache* pipelineCache) {
    if (!pipelineCache) return;
    if (atomic_load(&pipelineCache->createdSinceSave) > 0) PersistentPipelineCache_save(pipelineCache);
    vulkanWrapper.vkDestroyPipelineCache(pipelineCache->device, pipelineCache->cache, NULL);
    MEMFREE(pipelineCache);
}
