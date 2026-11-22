#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include "texture_decoder.h"
#include "vk_context.h"
#include "vulkan_helper.h"
#include "bc_decoder.h"
#include "string_utils.h"
#include "file_utils.h"

#define CACHE_DIR APP_CACHE_DIR "/vortek"
#define CACHE_MIN_IMAGE_WIDTH 1024
#define STAGING_ALIGNMENT 256
#define SMALL_REGION_HEIGHT 256
#define FENCE_WAIT_SLICE_NS 2000000000ull
#define FENCE_WAIT_LIMIT_NS 20000000000ull
#define CACHE_WRITER_MAX_PENDING_BYTES (64u << 20)

#define VK_FORMAT_BC_FLAG(f) (1u << (f - VK_FORMAT_BC1_RGB_UNORM_BLOCK))

static inline uint64_t monotonicNanos() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* ---------------------------------------------------------------------------------------------
 * Formats
 * ------------------------------------------------------------------------------------------- */
bool TextureDecoder_canDecodeFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
            return true;
        default:
            return false;
    }
}

static void getBCInfo(VkFormat format, int* bcN, int* blockSize, bool* isNoAlphaU) {
    *isNoAlphaU = false;
    switch (format) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
            *isNoAlphaU = true;
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
            *blockSize = 8;
            *bcN = 1;
            break;
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
            *blockSize = 16;
            *bcN = 2;
            break;
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
            *blockSize = 16;
            *bcN = 3;
            break;
        case VK_FORMAT_BC4_UNORM_BLOCK:
            *isNoAlphaU = true;
        case VK_FORMAT_BC4_SNORM_BLOCK:
            *blockSize = 8;
            *bcN = 4;
            break;
        case VK_FORMAT_BC5_UNORM_BLOCK:
            *isNoAlphaU = true;
        case VK_FORMAT_BC5_SNORM_BLOCK:
            *blockSize = 16;
            *bcN = 5;
            break;
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
            *isNoAlphaU = true; /* unsigned */
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
            *blockSize = 16;
            *bcN = 6;
            break;
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
            *blockSize = 16;
            *bcN = 7;
            break;
        default:
            *blockSize = 0;
            *bcN = 0;
            break;
    }
}

static uint32_t getDeviceSupportedFormats(VkPhysicalDevice physicalDevice) {
    VkFormatProperties formatProperties;
    uint32_t flags = 0;
    for (VkFormat format = VK_FORMAT_BC1_RGB_UNORM_BLOCK; format <= VK_FORMAT_BC7_SRGB_BLOCK; format++) {
        vulkanWrapper.vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &formatProperties);
        if (formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) flags |= VK_FORMAT_BC_FLAG(format);
    }
    return flags;
}

VkFormat TextureDecoder_getDecompressedFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
            return VK_FORMAT_B8G8R8A8_SRGB;
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_BC4_UNORM_BLOCK:
            return VK_FORMAT_R8_UNORM;
        case VK_FORMAT_BC4_SNORM_BLOCK:
            return VK_FORMAT_R8_SNORM;
        case VK_FORMAT_BC5_UNORM_BLOCK:
            return VK_FORMAT_R8G8_UNORM;
        case VK_FORMAT_BC5_SNORM_BLOCK:
            return VK_FORMAT_R8G8_SNORM;
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case VK_FORMAT_BC7_UNORM_BLOCK:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_BC7_SRGB_BLOCK:
            return VK_FORMAT_R8G8B8A8_SRGB;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

uint32_t TextureDecoder_getDecompressedTexelSize(VkFormat compressedFormat) {
    int blockSize, bcN;
    bool isNoAlphaU;
    getBCInfo(compressedFormat, &bcN, &blockSize, &isNoAlphaU);
    return bcN > 0 ? (uint32_t)BCDecoder_getOutputTexelSize(bcN) : 4;
}

static void internalDestroyImage(VkDevice device, TextureDecoder_Image* targetImage) {
    if (targetImage->image) vulkanWrapper.vkDestroyImage(device, targetImage->image, NULL);
    MEMFREE(targetImage);
}

/* ---------------------------------------------------------------------------------------------
 * Persistent cache of decoded regions (only regions >= CACHE_MIN_IMAGE_WIDTH in both axes).
 * Reads happen on the request thread (needed right away); writes are handed to a background
 * writer thread so the game's submit never waits for flash storage.
 * ------------------------------------------------------------------------------------------- */
typedef struct CacheWriteJob {
    char filename[256];
    void* data;
    size_t size;
    uint64_t cacheSizeBytes;
    struct CacheWriteJob* next;
} CacheWriteJob;

static pthread_mutex_t cacheWriterMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cacheWriterCond = PTHREAD_COND_INITIALIZER;
static CacheWriteJob* cacheWriterHead = NULL;
static CacheWriteJob* cacheWriterTail = NULL;
static size_t cacheWriterPendingBytes = 0;
static bool cacheWriterStarted = false;

static void* cacheWriterThread(void* param) {
    (void)param;
    while (true) {
        pthread_mutex_lock(&cacheWriterMutex);
        while (!cacheWriterHead) pthread_cond_wait(&cacheWriterCond, &cacheWriterMutex);
        CacheWriteJob* job = cacheWriterHead;
        cacheWriterHead = job->next;
        if (!cacheWriterHead) cacheWriterTail = NULL;
        pthread_mutex_unlock(&cacheWriterMutex);

        char tmpFilename[272];
        snprintf(tmpFilename, sizeof(tmpFilename), "%s.tmp", job->filename);
        if (filePutContents(tmpFilename, job->data, job->size)) rename(tmpFilename, job->filename);

        char value[32];
        snprintf(value, sizeof(value), "%llu", (unsigned long long)job->cacheSizeBytes);
        filePutContents(CACHE_DIR "/.cache-size", value, strlen(value));

        pthread_mutex_lock(&cacheWriterMutex);
        cacheWriterPendingBytes -= job->size;
        pthread_mutex_unlock(&cacheWriterMutex);
        MEMFREE(job->data);
        MEMFREE(job);
    }
    return NULL;
}

static void getCacheFilename(char* filename, size_t length, uint64_t hash, uint32_t width, uint32_t height, VkFormat format) {
    snprintf(filename, length, CACHE_DIR "/%lx-%ux%u-%d.imd", (unsigned long)hash, width, height, format);
}

static bool readCachedRegion(uint64_t hash, uint32_t width, uint32_t height, VkFormat format, void* result, size_t size) {
    char filename[256];
    getCacheFilename(filename, sizeof(filename), hash, width, height, format);
    struct stat st;
    if (stat(filename, &st) != 0 || (size_t)st.st_size != size) return false;
    size_t fileSize = size;
    return fileGetContents(filename, result, &fileSize) != NULL;
}

static void loadCacheSize(TextureDecoder* textureDecoder) {
    if (textureDecoder->cacheSizeLoaded) return;
    textureDecoder->cacheSizeLoaded = true;
    char* content = fileGetContents(CACHE_DIR "/.cache-size", NULL, NULL);
    if (content) {
        textureDecoder->cacheSizeBytes = strtoull(content, NULL, 10);
        MEMFREE(content);
    }
}

static void writeCachedRegion(TextureDecoder* textureDecoder, uint64_t hash, uint32_t width, uint32_t height, VkFormat format, const void* data, size_t size) {
    loadCacheSize(textureDecoder);
    uint64_t maxCacheSize = (uint64_t)textureDecoder->imageCacheSize << 20;
    if (textureDecoder->cacheSizeBytes + size > maxCacheSize) return;

    pthread_mutex_lock(&cacheWriterMutex);
    bool accept = cacheWriterPendingBytes + size <= CACHE_WRITER_MAX_PENDING_BYTES;
    if (accept) cacheWriterPendingBytes += size;
    if (accept && !cacheWriterStarted) {
        mkdir(CACHE_DIR, 0771);
        pthread_t thread;
        cacheWriterStarted = pthread_create(&thread, NULL, cacheWriterThread, NULL) == 0;
        if (cacheWriterStarted) pthread_detach(thread);
        else {
            accept = false;
            cacheWriterPendingBytes -= size;
        }
    }
    pthread_mutex_unlock(&cacheWriterMutex);
    if (!accept) return;

    CacheWriteJob* job = calloc(1, sizeof(CacheWriteJob));
    job->data = malloc(size);
    memcpy(job->data, data, size);
    job->size = size;
    getCacheFilename(job->filename, sizeof(job->filename), hash, width, height, format);
    textureDecoder->cacheSizeBytes += size;
    job->cacheSizeBytes = textureDecoder->cacheSizeBytes;

    pthread_mutex_lock(&cacheWriterMutex);
    if (cacheWriterTail) cacheWriterTail->next = job;
    else cacheWriterHead = job;
    cacheWriterTail = job;
    pthread_cond_signal(&cacheWriterCond);
    pthread_mutex_unlock(&cacheWriterMutex);
}

/* ---------------------------------------------------------------------------------------------
 * Region decoding (shared by every upload mode)
 * ------------------------------------------------------------------------------------------- */
typedef struct MappedMemory {
    ResourceMemory* memory;
    uint8_t* data;
} MappedMemory;

typedef struct MapCache {
    MappedMemory entries[16];
    int count;
} MapCache;

static uint8_t* MapCache_map(MapCache* cache, ResourceMemory* memory) {
    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i].memory == memory) return cache->entries[i].data;
    }
    uint8_t* data = mmap(NULL, memory->allocationSize, PROT_READ, MAP_SHARED, memory->fd, 0);
    if (data == MAP_FAILED) return NULL;
    if (cache->count == ARRAY_SIZE(cache->entries)) {
        munmap(cache->entries[0].data, cache->entries[0].memory->allocationSize);
        memmove(cache->entries, cache->entries + 1, sizeof(MappedMemory) * (cache->count - 1));
        cache->count--;
    }
    cache->entries[cache->count].memory = memory;
    cache->entries[cache->count].data = data;
    cache->count++;
    return data;
}

static void MapCache_release(MapCache* cache) {
    for (int i = 0; i < cache->count; i++) munmap(cache->entries[i].data, cache->entries[i].memory->allocationSize);
    cache->count = 0;
}

typedef struct DecodeItem {
    const uint8_t* src;
    uint8_t* dst;
    uint8_t* ownedSrc;
    uint32_t width;
    uint32_t height;
    VkFormat format;
    int bcN;
    bool isNoAlphaU;
    bool cacheable;
    uint64_t hash;
    size_t decodedSize;
    TaskGroup* group;
} DecodeItem;

static void decodeItemThread(void* param) {
    DecodeItem* item = param;
    BCDecoder_decode(item->src, item->dst, item->width, item->height, item->bcN, item->isNoAlphaU, NULL);
    TaskGroup_done(item->group);
}

/*
 * Prepares the decode of one copy region into `dst` (tightly packed decoded texels, slice after
 * slice). Honors bufferRowLength / bufferImageHeight of the compressed source (partial updates of
 * larger textures). Cached slices are read directly into `dst`; the others are appended to `items`.
 */
static bool prepareRegion(TextureDecoder* textureDecoder, MapCache* mapCache, TextureDecoder_BoundBuffer* srcBuffer, TextureDecoder_Image* dstImage, const VkBufferImageCopy* region, uint8_t* dst, ArrayList* items) {
    if (!srcBuffer || !dstImage || !srcBuffer->memory) return false;

    int blockSize, bcN;
    bool isNoAlphaU;
    getBCInfo(dstImage->format, &bcN, &blockSize, &isNoAlphaU);
    if (bcN == 0) return false;

    const uint32_t width = region->imageExtent.width;
    const uint32_t height = region->imageExtent.height;
    const uint32_t slices = MAX(region->imageExtent.depth, 1) * MAX(region->imageSubresource.layerCount, 1);
    if (width == 0 || height == 0) return false;

    const uint32_t rowLength = region->bufferRowLength > 0 ? region->bufferRowLength : width;
    const uint32_t imageHeight = region->bufferImageHeight > 0 ? region->bufferImageHeight : height;
    const VkDeviceSize tightRowPitch = (VkDeviceSize)((width + 3) / 4) * blockSize;
    const VkDeviceSize tightBlockRows = (height + 3) / 4;
    const VkDeviceSize tightSliceSize = tightRowPitch * tightBlockRows;
    const VkDeviceSize srcRowPitch = (VkDeviceSize)((rowLength + 3) / 4) * blockSize;
    const VkDeviceSize srcSlicePitch = srcRowPitch * ((imageHeight + 3) / 4);
    const bool repack = srcRowPitch != tightRowPitch || srcSlicePitch != tightSliceSize;

    const VkDeviceSize sourceOffset = srcBuffer->memoryOffset + region->bufferOffset;
    const VkDeviceSize lastByte = sourceOffset + (VkDeviceSize)(slices - 1) * srcSlicePitch + (tightBlockRows - 1) * srcRowPitch + tightRowPitch;
    if (lastByte > srcBuffer->memory->allocationSize) {
        println("vortek: texture decoder: copy region outside of the source memory (%llu > %llu), skipped",
                (unsigned long long)lastByte, (unsigned long long)srcBuffer->memory->allocationSize);
        return false;
    }

    uint8_t* bufferData = MapCache_map(mapCache, srcBuffer->memory);
    if (!bufferData) return false;

    const uint32_t texelSize = BCDecoder_getOutputTexelSize(bcN);
    const size_t decodedSliceSize = (size_t)width * height * texelSize;
    const bool cacheable = width >= CACHE_MIN_IMAGE_WIDTH && height >= CACHE_MIN_IMAGE_WIDTH;

    for (uint32_t s = 0; s < slices; s++) {
        const uint8_t* src = bufferData + sourceOffset + s * srcSlicePitch;
        uint8_t* ownedSrc = NULL;
        if (repack) {
            ownedSrc = malloc(tightSliceSize);
            for (VkDeviceSize row = 0; row < tightBlockRows; row++) memcpy(ownedSrc + row * tightRowPitch, src + row * srcRowPitch, tightRowPitch);
            src = ownedSrc;
        }

        uint8_t* sliceDst = dst + s * decodedSliceSize;
        uint64_t hash = 0;
        if (cacheable) {
            hash = murmurHash64((const char*)src, tightSliceSize, dstImage->format);
            if (readCachedRegion(hash, width, height, dstImage->format, sliceDst, decodedSliceSize)) {
                textureDecoder->statCacheHits++;
                MEMFREE(ownedSrc);
                continue;
            }
        }

        DecodeItem* item = calloc(1, sizeof(DecodeItem));
        item->src = src;
        item->dst = sliceDst;
        item->ownedSrc = ownedSrc;
        item->width = width;
        item->height = height;
        item->format = dstImage->format;
        item->bcN = bcN;
        item->isNoAlphaU = isNoAlphaU;
        item->cacheable = cacheable;
        item->hash = hash;
        item->decodedSize = decodedSliceSize;
        ArrayList_add(items, item);
    }

    textureDecoder->statRegions++;
    return true;
}

/*
 * Decodes every prepared item. Large regions are split by rows across the decode pool; small
 * regions (typically mip levels) are decoded one per worker. Waits only for its own tasks.
 */
static void runDecodeItems(TextureDecoder* textureDecoder, ArrayList* items) {
    if (items->size == 0) return;
    uint64_t start = monotonicNanos();

    TaskGroup group;
    TaskGroup_init(&group);
    ThreadPool* pool = textureDecoder->threadPool;

    for (int i = 0; i < items->size; i++) {
        DecodeItem* item = items->elements[i];
        if (!pool || item->height >= SMALL_REGION_HEIGHT) continue;
        item->group = &group;
        TaskGroup_add(&group, 1);
        ThreadPool_run(pool, decodeItemThread, item);
    }

    for (int i = 0; i < items->size; i++) {
        DecodeItem* item = items->elements[i];
        if (pool && item->height < SMALL_REGION_HEIGHT) continue;
        BCDecoder_decode(item->src, item->dst, item->width, item->height, item->bcN, item->isNoAlphaU, pool);
    }

    TaskGroup_wait(&group);
    TaskGroup_destroy(&group);

    for (int i = 0; i < items->size; i++) {
        DecodeItem* item = items->elements[i];
        if (item->cacheable) writeCachedRegion(textureDecoder, item->hash, item->width, item->height, item->format, item->dst, item->decodedSize);
        MEMFREE(item->ownedSrc);
        MEMFREE(item);
    }
    items->size = 0;

    textureDecoder->statDecodeNanos += monotonicNanos() - start;
}

static VkDeviceSize getDecodedRegionSize(TextureDecoder_Image* dstImage, const VkBufferImageCopy* region) {
    const uint32_t slices = MAX(region->imageExtent.depth, 1) * MAX(region->imageSubresource.layerCount, 1);
    return (VkDeviceSize)region->imageExtent.width * region->imageExtent.height * TextureDecoder_getDecompressedTexelSize(dstImage->format) * slices;
}

/* ---------------------------------------------------------------------------------------------
 * Host-visible staging buffers
 * ------------------------------------------------------------------------------------------- */
static uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) {
    for (uint32_t i = 0; i < 32; i++) {
        if ((typeBits & (1u << i)) && (getMemoryPropertyFlags(i) & required) == required) return i;
    }
    return UINT32_MAX;
}

static bool createHostBuffer(TextureDecoder* textureDecoder, VkDeviceSize size, VkBuffer* outBuffer, VkDeviceMemory* outMemory, uint8_t** outMapped, bool* outCoherent) {
    *outBuffer = VK_NULL_HANDLE;
    *outMemory = VK_NULL_HANDLE;
    *outMapped = NULL;

    VkBufferCreateInfo bufferInfo = {0};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VkResult result = vulkanWrapper.vkCreateBuffer(textureDecoder->device, &bufferInfo, NULL, outBuffer);
    if (result != VK_SUCCESS) goto error;

    VkMemoryRequirements memReqs = {0};
    vulkanWrapper.vkGetBufferMemoryRequirements(textureDecoder->device, *outBuffer, &memReqs);

    uint32_t memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    *outCoherent = memoryTypeIndex != UINT32_MAX;
    if (!*outCoherent) memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (memoryTypeIndex == UINT32_MAX) memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

    VkMemoryAllocateInfo allocateInfo = {0};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = memReqs.size;
    allocateInfo.memoryTypeIndex = memoryTypeIndex;

    result = vulkanWrapper.vkAllocateMemory(textureDecoder->device, &allocateInfo, NULL, outMemory);
    if (result != VK_SUCCESS) goto error;

    result = vulkanWrapper.vkBindBufferMemory(textureDecoder->device, *outBuffer, *outMemory, 0);
    if (result != VK_SUCCESS) goto error;

    void* mapped = NULL;
    result = vulkanWrapper.vkMapMemory(textureDecoder->device, *outMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result != VK_SUCCESS) goto error;

    *outMapped = mapped;
    textureDecoder->statStagingBytes += size;
    return true;

error:
    println("vortek: texture decoder: unable to create a staging buffer of %llu bytes (result %d)", (unsigned long long)size, result);
    if (*outBuffer) vulkanWrapper.vkDestroyBuffer(textureDecoder->device, *outBuffer, NULL);
    if (*outMemory) vulkanWrapper.vkFreeMemory(textureDecoder->device, *outMemory, NULL);
    *outBuffer = VK_NULL_HANDLE;
    *outMemory = VK_NULL_HANDLE;
    return false;
}

static void destroyHostBuffer(TextureDecoder* textureDecoder, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize size) {
    if (memory) vulkanWrapper.vkUnmapMemory(textureDecoder->device, memory);
    if (buffer) vulkanWrapper.vkDestroyBuffer(textureDecoder->device, buffer, NULL);
    if (memory) vulkanWrapper.vkFreeMemory(textureDecoder->device, memory, NULL);
    if (textureDecoder->statStagingBytes >= size) textureDecoder->statStagingBytes -= size;
}

static void flushHostMemory(TextureDecoder* textureDecoder, VkDeviceMemory memory) {
    VkMappedMemoryRange range = {0};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = memory;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    vulkanWrapper.vkFlushMappedMemoryRanges(textureDecoder->device, 1, &range);
}

/* Waits for a fence in bounded slices: a hung GPU is reported instead of blocking forever. */
static VkResult waitFenceBounded(TextureDecoder* textureDecoder, VkFence fence, const char* what) {
    uint64_t waited = 0;
    while (true) {
        VkResult result = vulkanWrapper.vkWaitForFences(textureDecoder->device, 1, &fence, VK_TRUE, FENCE_WAIT_SLICE_NS);
        if (result != VK_TIMEOUT) return result;
        waited += FENCE_WAIT_SLICE_NS;
        println("vortek: texture decoder: %s still not finished after %llu s", what, (unsigned long long)(waited / 1000000000ull));
        if (waited >= FENCE_WAIT_LIMIT_NS) return VK_ERROR_DEVICE_LOST;
    }
}

/* ---------------------------------------------------------------------------------------------
 * Legacy modes (BATCHED_SAFE / BATCHED / SYNC): copies hoisted before the game's submit
 * ------------------------------------------------------------------------------------------- */
static void destroyStagingBuffer(TextureDecoder* textureDecoder, TextureDecoder_StagingSlot* slot) {
    destroyHostBuffer(textureDecoder, slot->buffer, slot->memory, slot->capacity);
    slot->buffer = VK_NULL_HANDLE;
    slot->memory = VK_NULL_HANDLE;
    slot->mapped = NULL;
    slot->capacity = 0;
    slot->used = 0;
}

static bool createStagingBuffer(TextureDecoder* textureDecoder, TextureDecoder_StagingSlot* slot, VkDeviceSize size) {
    destroyStagingBuffer(textureDecoder, slot);
    if (!createHostBuffer(textureDecoder, size, &slot->buffer, &slot->memory, &slot->mapped, &slot->coherent)) return false;
    slot->capacity = size;
    slot->used = 0;
    return true;
}

static VkResult waitSlot(TextureDecoder* textureDecoder, TextureDecoder_StagingSlot* slot) {
    if (!slot->pending) return VK_SUCCESS;
    VkResult result = VK_SUCCESS;
    if (vulkanWrapper.vkGetFenceStatus(textureDecoder->device, slot->fence) != VK_SUCCESS) {
        result = waitFenceBounded(textureDecoder, slot->fence, "staging upload");
        textureDecoder->statSlotWaits++;
    }
    vulkanWrapper.vkResetFences(textureDecoder->device, 1, &slot->fence);
    slot->pending = false;
    return result;
}

static VkResult submitSlot(TextureDecoder* textureDecoder, TextureDecoder_StagingSlot* slot) {
    if (!slot->recording) return VK_SUCCESS;

    // Make the uploads available/visible to every command submitted later on this queue.
    VkMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vulkanWrapper.vkCmdPipelineBarrier(slot->commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    vulkanWrapper.vkEndCommandBuffer(slot->commandBuffer);

    if (!slot->coherent) flushHostMemory(textureDecoder, slot->memory);

    VkSubmitInfo submitInfo = {0};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &slot->commandBuffer;

    VkResult result = vulkanWrapper.vkQueueSubmit(textureDecoder->queue, 1, &submitInfo, slot->fence);
    slot->recording = false;
    slot->pending = result == VK_SUCCESS;
    textureDecoder->statSubmits++;
    textureDecoder->slotImages.size = 0;
    return result;
}

static TextureDecoder_StagingSlot* acquireStaging(TextureDecoder* textureDecoder, VkDeviceSize size, VkDeviceSize* outOffset, VkResult* outResult) {
    TextureDecoder_StagingSlot* slot = &textureDecoder->slots[textureDecoder->currentSlot];
    VkDeviceSize offset = (slot->used + STAGING_ALIGNMENT - 1) & ~(VkDeviceSize)(STAGING_ALIGNMENT - 1);
    *outResult = VK_SUCCESS;

    if (slot->recording && offset + size > slot->capacity) {
        *outResult = submitSlot(textureDecoder, slot);
        if (*outResult != VK_SUCCESS) return NULL;
        textureDecoder->currentSlot = (textureDecoder->currentSlot + 1) % TEXTURE_DECODER_STAGING_SLOTS;
        slot = &textureDecoder->slots[textureDecoder->currentSlot];
        offset = 0;
    }

    if (!slot->recording) {
        *outResult = waitSlot(textureDecoder, slot);
        if (*outResult != VK_SUCCESS) return NULL;
        slot->used = 0;
        offset = 0;

        if (size > slot->capacity) {
            VkDeviceSize capacity = size > TEXTURE_DECODER_MIN_STAGING_SIZE ? size : TEXTURE_DECODER_MIN_STAGING_SIZE;
            if (!createStagingBuffer(textureDecoder, slot, capacity)) return NULL;
        }

        VkCommandBufferBeginInfo beginInfo = {0};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vulkanWrapper.vkResetCommandBuffer(slot->commandBuffer, 0);
        vulkanWrapper.vkBeginCommandBuffer(slot->commandBuffer, &beginInfo);
        slot->recording = true;
        textureDecoder->slotImages.size = 0;
    }

    *outOffset = offset;
    return slot;
}

static void destroySlots(TextureDecoder* textureDecoder) {
    for (int i = 0; i < TEXTURE_DECODER_STAGING_SLOTS; i++) {
        TextureDecoder_StagingSlot* slot = &textureDecoder->slots[i];
        if (slot->recording) {
            vulkanWrapper.vkEndCommandBuffer(slot->commandBuffer);
            slot->recording = false;
        }
        if (slot->pending) waitSlot(textureDecoder, slot);
        if (slot->fence) {
            vulkanWrapper.vkDestroyFence(textureDecoder->device, slot->fence, NULL);
            slot->fence = VK_NULL_HANDLE;
        }
        destroyStagingBuffer(textureDecoder, slot);
    }

    if (textureDecoder->commandPool) {
        vulkanWrapper.vkDestroyCommandPool(textureDecoder->device, textureDecoder->commandPool, NULL);
        textureDecoder->commandPool = VK_NULL_HANDLE;
    }
}

static bool createSlots(TextureDecoder* textureDecoder, uint32_t graphicsQueueIndex) {
    vulkanWrapper.vkGetDeviceQueue(textureDecoder->device, graphicsQueueIndex, 0, &textureDecoder->queue);

    VkCommandPoolCreateInfo poolInfo = {0};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueIndex;
    if (vulkanWrapper.vkCreateCommandPool(textureDecoder->device, &poolInfo, NULL, &textureDecoder->commandPool) != VK_SUCCESS) goto error;

    for (int i = 0; i < TEXTURE_DECODER_STAGING_SLOTS; i++) {
        TextureDecoder_StagingSlot* slot = &textureDecoder->slots[i];

        VkCommandBufferAllocateInfo commandBufferInfo = {0};
        commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandBufferInfo.commandPool = textureDecoder->commandPool;
        commandBufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandBufferInfo.commandBufferCount = 1;
        if (vulkanWrapper.vkAllocateCommandBuffers(textureDecoder->device, &commandBufferInfo, &slot->commandBuffer) != VK_SUCCESS) goto error;

        VkFenceCreateInfo fenceInfo = {0};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vulkanWrapper.vkCreateFence(textureDecoder->device, &fenceInfo, NULL, &slot->fence) != VK_SUCCESS) goto error;
    }

    textureDecoder->currentSlot = 0;
    return true;

error:
    println("vortek: texture decoder: unable to create the upload command pool");
    destroySlots(textureDecoder);
    return false;
}

static VkResult decodeQueuedCopies(TextureDecoder* textureDecoder, VkQueue submitQueue) {
    if (ArrayDeque_isEmpty(&textureDecoder->bufferImageCopies)) return VK_SUCCESS;

    if (!textureDecoder->commandPool) {
        // Initialization failed: drop the pending copies instead of touching invalid objects.
        TextureDecoder_BufferImageCopy* pending;
        while (!ArrayDeque_isEmpty(&textureDecoder->bufferImageCopies) && (pending = ArrayDeque_removeFirst(&textureDecoder->bufferImageCopies))) MEMFREE(pending);
        return VK_SUCCESS;
    }

    const bool sync = textureDecoder->uploadMode == TEXTURE_UPLOAD_MODE_SYNC;
    const bool safe = textureDecoder->uploadMode != TEXTURE_UPLOAD_MODE_BATCHED;
    VkResult result = VK_SUCCESS;
    bool recorded = false;
    MapCache mapCache = {0};
    ArrayList items = {0};

    while (!ArrayDeque_isEmpty(&textureDecoder->bufferImageCopies)) {
        TextureDecoder_BufferImageCopy* bufferImageCopy = ArrayDeque_removeFirst(&textureDecoder->bufferImageCopies);
        if (!bufferImageCopy) break;

        TextureDecoder_BoundBuffer* srcBuffer = bufferImageCopy->srcBuffer;
        TextureDecoder_Image* dstImage = bufferImageCopy->dstImage;
        VkBufferImageCopy region = bufferImageCopy->region;
        VkImageLayout dstImageLayout = bufferImageCopy->dstImageLayout;
        MEMFREE(bufferImageCopy);
        if (!srcBuffer || !dstImage || !srcBuffer->memory) continue;

        VkDeviceSize totalSize = getDecodedRegionSize(dstImage, &region);
        if (totalSize == 0) continue;

        VkDeviceSize stagingOffset = 0;
        TextureDecoder_StagingSlot* slot = acquireStaging(textureDecoder, totalSize, &stagingOffset, &result);
        if (!slot) {
            if (result != VK_SUCCESS) break;
            continue;
        }

        if (!prepareRegion(textureDecoder, &mapCache, srcBuffer, dstImage, &region, slot->mapped + stagingOffset, &items)) continue;
        runDecodeItems(textureDecoder, &items);

        // BATCHED_SAFE / SYNC: a second write to the same image in this batch must wait for the first.
        if (safe) {
            if (ArrayList_indexOf(&textureDecoder->slotImages, dstImage) >= 0) {
                VkMemoryBarrier barrier = {0};
                barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vulkanWrapper.vkCmdPipelineBarrier(slot->commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
                textureDecoder->slotImages.size = 0;
                textureDecoder->statWAWBarriers++;
            }
            ArrayList_add(&textureDecoder->slotImages, dstImage);
        }

        region.bufferOffset = stagingOffset;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        vulkanWrapper.vkCmdCopyBufferToImage(slot->commandBuffer, slot->buffer, dstImage->image, dstImageLayout, 1, &region);
        slot->used = stagingOffset + totalSize;
        recorded = true;

        if (sync) {
            // Winlator 11.2 semantics: one submit and one blocking wait per region.
            result = submitSlot(textureDecoder, slot);
            if (result == VK_SUCCESS) result = waitSlot(textureDecoder, slot);
            if (result != VK_SUCCESS) break;
            recorded = false;
        }
    }

    MapCache_release(&mapCache);
    ArrayList_free(&items, false);
    if (result != VK_SUCCESS) {
        TextureDecoder_BufferImageCopy* pending;
        while (!ArrayDeque_isEmpty(&textureDecoder->bufferImageCopies) && (pending = ArrayDeque_removeFirst(&textureDecoder->bufferImageCopies))) MEMFREE(pending);
        return result;
    }
    if (!recorded) return VK_SUCCESS;

    TextureDecoder_StagingSlot* slot = &textureDecoder->slots[textureDecoder->currentSlot];
    result = submitSlot(textureDecoder, slot);
    if (result != VK_SUCCESS) return result;

    // Submission order + the memory barrier guarantee that the uploads complete before the game's
    // commands only on the SAME queue. Otherwise keep the old blocking behavior for correctness.
    if (submitQueue != textureDecoder->queue) {
        for (int i = 0; i < TEXTURE_DECODER_STAGING_SLOTS; i++) {
            result = waitSlot(textureDecoder, &textureDecoder->slots[i]);
            if (result != VK_SUCCESS) return result;
        }
    }
    textureDecoder->currentSlot = (textureDecoder->currentSlot + 1) % TEXTURE_DECODER_STAGING_SLOTS;
    return VK_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------
 * INLINE mode
 * ------------------------------------------------------------------------------------------- */
static void destroyChunk(TextureDecoder* textureDecoder, TextureDecoder_StagingChunk* chunk) {
    destroyHostBuffer(textureDecoder, chunk->buffer, chunk->memory, chunk->capacity);
    MEMFREE(chunk);
}

static TextureDecoder_StagingChunk* takeChunk(TextureDecoder* textureDecoder, VkDeviceSize size) {
    int best = -1;
    for (int i = 0; i < textureDecoder->freeChunks.size; i++) {
        TextureDecoder_StagingChunk* chunk = textureDecoder->freeChunks.elements[i];
        if (chunk->capacity >= size && (best == -1 || chunk->capacity < ((TextureDecoder_StagingChunk*)textureDecoder->freeChunks.elements[best])->capacity)) best = i;
    }

    if (best >= 0) {
        TextureDecoder_StagingChunk* chunk = ArrayList_removeAt(&textureDecoder->freeChunks, best);
        textureDecoder->freeChunkBytes -= chunk->capacity;
        chunk->used = 0;
        return chunk;
    }

    TextureDecoder_StagingChunk* chunk = calloc(1, sizeof(TextureDecoder_StagingChunk));
    VkDeviceSize capacity = size > TEXTURE_DECODER_CHUNK_SIZE ? size : TEXTURE_DECODER_CHUNK_SIZE;
    if (!createHostBuffer(textureDecoder, capacity, &chunk->buffer, &chunk->memory, &chunk->mapped, &chunk->coherent)) {
        MEMFREE(chunk);
        return NULL;
    }
    chunk->capacity = capacity;
    return chunk;
}

static void returnChunk(TextureDecoder* textureDecoder, TextureDecoder_StagingChunk* chunk) {
    if (textureDecoder->freeChunkBytes + chunk->capacity > TEXTURE_DECODER_MAX_FREE_CHUNK_BYTES) {
        destroyChunk(textureDecoder, chunk);
        return;
    }
    chunk->used = 0;
    ArrayList_add(&textureDecoder->freeChunks, chunk);
    textureDecoder->freeChunkBytes += chunk->capacity;
}

static TextureDecoder_CommandBufferState* getCommandBufferState(TextureDecoder* textureDecoder, VkCommandBuffer commandBuffer, bool create) {
    TextureDecoder_CommandBufferState* state = SparseArray64_get(&textureDecoder->commandBuffers, (int64_t)commandBuffer);
    if (!state && create) {
        state = calloc(1, sizeof(TextureDecoder_CommandBufferState));
        state->commandBuffer = commandBuffer;
        SparseArray64_put(&textureDecoder->commandBuffers, (int64_t)commandBuffer, state);
    }
    return state;
}

/* Called when the command buffer is reset (explicitly, by vkBeginCommandBuffer or by its pool) or
 * freed. Vulkan forbids these while the command buffer is pending, so the staging memory is idle. */
static void releaseCommandBufferState(TextureDecoder* textureDecoder, TextureDecoder_CommandBufferState* state) {
    for (int i = 0; i < state->jobs.size; i++) MEMFREE(state->jobs.elements[i]);
    state->jobs.size = 0;
    for (int i = 0; i < state->chunks.size; i++) returnChunk(textureDecoder, state->chunks.elements[i]);
    state->chunks.size = 0;
    state->secondaries.size = 0;
    state->decoded = false;
}

static void freeCommandBufferState(TextureDecoder* textureDecoder, TextureDecoder_CommandBufferState* state) {
    releaseCommandBufferState(textureDecoder, state);
    ArrayList_free(&state->jobs, false);
    ArrayList_free(&state->chunks, false);
    ArrayList_free(&state->secondaries, false);
    MEMFREE(state);
}

static bool allocInlineStaging(TextureDecoder* textureDecoder, TextureDecoder_CommandBufferState* state, VkDeviceSize size, TextureDecoder_StagingChunk** outChunk, VkDeviceSize* outOffset) {
    if (state->chunks.size > 0) {
        TextureDecoder_StagingChunk* chunk = state->chunks.elements[state->chunks.size - 1];
        VkDeviceSize offset = (chunk->used + STAGING_ALIGNMENT - 1) & ~(VkDeviceSize)(STAGING_ALIGNMENT - 1);
        if (offset + size <= chunk->capacity) {
            chunk->used = offset + size;
            *outChunk = chunk;
            *outOffset = offset;
            return true;
        }
    }

    TextureDecoder_StagingChunk* chunk = takeChunk(textureDecoder, size);
    if (!chunk) return false;
    ArrayList_add(&state->chunks, chunk);
    chunk->used = size;
    *outChunk = chunk;
    *outOffset = 0;
    return true;
}

static void queueLegacyCopy(TextureDecoder* textureDecoder, TextureDecoder_BoundBuffer* boundBuffer, TextureDecoder_Image* targetImage, VkImageLayout dstImageLayout, const VkBufferImageCopy* region) {
    TextureDecoder_BufferImageCopy* bufferImageCopy = calloc(1, sizeof(TextureDecoder_BufferImageCopy));
    bufferImageCopy->srcBuffer = boundBuffer;
    bufferImageCopy->dstImage = targetImage;
    bufferImageCopy->dstImageLayout = dstImageLayout;
    memcpy(&bufferImageCopy->region, region, sizeof(VkBufferImageCopy));
    ArrayDeque_addLast(&textureDecoder->bufferImageCopies, bufferImageCopy);
}

static void recordCopy(TextureDecoder* textureDecoder, VkCommandBuffer commandBuffer, TextureDecoder_BoundBuffer* boundBuffer, TextureDecoder_Image* targetImage, VkImageLayout dstImageLayout, const VkBufferImageCopy* region) {
    if (textureDecoder->uploadMode != TEXTURE_UPLOAD_MODE_INLINE) {
        queueLegacyCopy(textureDecoder, boundBuffer, targetImage, dstImageLayout, region);
        return;
    }

    VkDeviceSize size = getDecodedRegionSize(targetImage, region);
    if (size == 0) return;

    TextureDecoder_CommandBufferState* state = getCommandBufferState(textureDecoder, commandBuffer, true);
    TextureDecoder_StagingChunk* chunk = NULL;
    VkDeviceSize stagingOffset = 0;
    if (!allocInlineStaging(textureDecoder, state, size, &chunk, &stagingOffset)) {
        // Out of staging memory: fall back to the batched path rather than losing the texture.
        println("vortek: texture decoder: inline staging allocation failed (%llu bytes), using the batched path", (unsigned long long)size);
        queueLegacyCopy(textureDecoder, boundBuffer, targetImage, dstImageLayout, region);
        return;
    }

    TextureDecoder_InlineJob* job = calloc(1, sizeof(TextureDecoder_InlineJob));
    job->srcBuffer = boundBuffer;
    job->dstImage = targetImage;
    job->srcRegion = *region;
    job->chunk = chunk;
    job->stagingOffset = stagingOffset;
    ArrayList_add(&state->jobs, job);
    state->decoded = false;

    VkBufferImageCopy decodedRegion = *region;
    decodedRegion.bufferOffset = stagingOffset;
    decodedRegion.bufferRowLength = 0;
    decodedRegion.bufferImageHeight = 0;
    vulkanWrapper.vkCmdCopyBufferToImage(commandBuffer, chunk->buffer, targetImage->image, dstImageLayout, 1, &decodedRegion);
}

static void collectCommandBufferJobs(TextureDecoder* textureDecoder, TextureDecoder_CommandBufferState* state, MapCache* mapCache, ArrayList* items, ArrayList* decodedStates, int depth) {
    if (!state || depth > 1) return;

    if (state->jobs.size > 0 && !(state->decoded && state->simultaneousUse)) {
        for (int i = 0; i < state->jobs.size; i++) {
            TextureDecoder_InlineJob* job = state->jobs.elements[i];
            if (!job->dstImage || !job->srcBuffer) continue;
            prepareRegion(textureDecoder, mapCache, job->srcBuffer, job->dstImage, &job->srcRegion, job->chunk->mapped + job->stagingOffset, items);
        }
        ArrayList_add(decodedStates, state);
    }

    for (int i = 0; i < state->secondaries.size; i++) {
        TextureDecoder_CommandBufferState* secondary = getCommandBufferState(textureDecoder, state->secondaries.elements[i], false);
        collectCommandBufferJobs(textureDecoder, secondary, mapCache, items, decodedStates, depth + 1);
    }
}

VkResult TextureDecoder_onQueueSubmit(TextureDecoder* textureDecoder, VkQueue submitQueue, uint32_t commandBufferCount, const VkCommandBuffer* commandBuffers) {
    VkResult result = decodeQueuedCopies(textureDecoder, submitQueue);
    if (result != VK_SUCCESS || textureDecoder->uploadMode != TEXTURE_UPLOAD_MODE_INLINE) return result;

    MapCache mapCache = {0};
    ArrayList items = {0};
    ArrayList decodedStates = {0};

    for (uint32_t i = 0; i < commandBufferCount; i++) {
        TextureDecoder_CommandBufferState* state = getCommandBufferState(textureDecoder, commandBuffers[i], false);
        collectCommandBufferJobs(textureDecoder, state, &mapCache, &items, &decodedStates, 0);
    }

    runDecodeItems(textureDecoder, &items);
    MapCache_release(&mapCache);

    for (int i = 0; i < decodedStates.size; i++) {
        TextureDecoder_CommandBufferState* state = decodedStates.elements[i];
        for (int j = 0; j < state->chunks.size; j++) {
            TextureDecoder_StagingChunk* chunk = state->chunks.elements[j];
            if (!chunk->coherent) flushHostMemory(textureDecoder, chunk->memory);
        }
        state->decoded = true;
        // One-time command buffers are never submitted again: drop the job list, keep the memory.
        if (state->oneTimeSubmit) {
            for (int j = 0; j < state->jobs.size; j++) MEMFREE(state->jobs.elements[j]);
            state->jobs.size = 0;
        }
    }

    ArrayList_free(&items, false);
    ArrayList_free(&decodedStates, false);
    return VK_SUCCESS;
}

void TextureDecoder_onAllocateCommandBuffers(TextureDecoder* textureDecoder, VkCommandPool commandPool, uint32_t count, const VkCommandBuffer* commandBuffers) {
    for (uint32_t i = 0; i < count; i++) {
        TextureDecoder_CommandBufferState* state = getCommandBufferState(textureDecoder, commandBuffers[i], true);
        releaseCommandBufferState(textureDecoder, state);
        state->commandPool = commandPool;
    }
}

void TextureDecoder_onFreeCommandBuffers(TextureDecoder* textureDecoder, uint32_t count, const VkCommandBuffer* commandBuffers) {
    for (uint32_t i = 0; i < count; i++) {
        TextureDecoder_CommandBufferState* state = SparseArray64_remove(&textureDecoder->commandBuffers, (int64_t)commandBuffers[i]);
        if (state) freeCommandBufferState(textureDecoder, state);
    }
}

void TextureDecoder_onBeginCommandBuffer(TextureDecoder* textureDecoder, VkCommandBuffer commandBuffer, VkCommandBufferUsageFlags flags) {
    TextureDecoder_CommandBufferState* state = getCommandBufferState(textureDecoder, commandBuffer, false);
    if (!state && textureDecoder->uploadMode != TEXTURE_UPLOAD_MODE_INLINE) return;
    if (!state) state = getCommandBufferState(textureDecoder, commandBuffer, true);
    releaseCommandBufferState(textureDecoder, state);
    state->oneTimeSubmit = (flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) != 0;
    state->simultaneousUse = (flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) != 0;
}

void TextureDecoder_onResetCommandBuffer(TextureDecoder* textureDecoder, VkCommandBuffer commandBuffer) {
    TextureDecoder_CommandBufferState* state = getCommandBufferState(textureDecoder, commandBuffer, false);
    if (state) releaseCommandBufferState(textureDecoder, state);
}

void TextureDecoder_onResetCommandPool(TextureDecoder* textureDecoder, VkCommandPool commandPool) {
    for (int i = 0; i < textureDecoder->commandBuffers.size; i++) {
        TextureDecoder_CommandBufferState* state = textureDecoder->commandBuffers.entries[i].value;
        if (state && state->commandPool == commandPool) releaseCommandBufferState(textureDecoder, state);
    }
}

void TextureDecoder_onDestroyCommandPool(TextureDecoder* textureDecoder, VkCommandPool commandPool) {
    for (int i = textureDecoder->commandBuffers.size - 1; i >= 0; i--) {
        TextureDecoder_CommandBufferState* state = textureDecoder->commandBuffers.entries[i].value;
        if (state && state->commandPool == commandPool) {
            SparseArray64_removeAt(&textureDecoder->commandBuffers, i);
            freeCommandBufferState(textureDecoder, state);
        }
    }
}

void TextureDecoder_onExecuteCommands(TextureDecoder* textureDecoder, VkCommandBuffer primary, uint32_t count, const VkCommandBuffer* secondaries) {
    if (textureDecoder->uploadMode != TEXTURE_UPLOAD_MODE_INLINE) return;
    TextureDecoder_CommandBufferState* state = getCommandBufferState(textureDecoder, primary, true);
    for (uint32_t i = 0; i < count; i++) ArrayList_add(&state->secondaries, secondaries[i]);
}

/* ---------------------------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------------------------- */
TextureDecoder* TextureDecoder_create(VkContext* context, VkPhysicalDevice physicalDevice, VkDevice device, VkPhysicalDeviceFeatures* supportedFeatures) {
    if (supportedFeatures->textureCompressionBC) return NULL;
    TextureDecoder* textureDecoder = calloc(1, sizeof(TextureDecoder));
    textureDecoder->context = context;
    textureDecoder->device = device;
    textureDecoder->imageCacheSize = context->imageCacheSize;
    textureDecoder->threadPool = context->decodeThreadPool;
    textureDecoder->uploadMode = context->textureUploadMode;
    if (textureDecoder->uploadMode < TEXTURE_UPLOAD_MODE_INLINE || textureDecoder->uploadMode > TEXTURE_UPLOAD_MODE_SYNC) textureDecoder->uploadMode = TEXTURE_UPLOAD_MODE_INLINE;

    createSlots(textureDecoder, context->graphicsQueueIndex);
    if (context->driverID == VK_DRIVER_ID_SAMSUNG_PROPRIETARY) textureDecoder->deviceSupportedFormats = getDeviceSupportedFormats(physicalDevice);

    static const char* modeNames[] = {"inline", "batched-safe", "batched", "sync"};
    println("vortek: texture decoder active (no native BC): upload mode %s", modeNames[textureDecoder->uploadMode]);
    return textureDecoder;
}

void TextureDecoder_destroy(TextureDecoder* textureDecoder) {
    for (int i = 0; i < textureDecoder->commandBuffers.size; i++) {
        TextureDecoder_CommandBufferState* state = textureDecoder->commandBuffers.entries[i].value;
        if (state) freeCommandBufferState(textureDecoder, state);
    }
    SparseArray64_free(&textureDecoder->commandBuffers, false);
    for (int i = 0; i < textureDecoder->freeChunks.size; i++) destroyChunk(textureDecoder, textureDecoder->freeChunks.elements[i]);
    ArrayList_free(&textureDecoder->freeChunks, false);

    destroySlots(textureDecoder);
    ArrayList_free(&textureDecoder->slotImages, false);

    SparseArray64_free(&textureDecoder->images, true);
    SparseArray64_free(&textureDecoder->boundBuffers, true);
    ArrayDeque_free(&textureDecoder->bufferImageCopies, true);
    MEMFREE(textureDecoder);
}

void TextureDecoder_copyBufferToImage(TextureDecoder* textureDecoder, VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, VkBufferImageCopy* regions) {
    TextureDecoder_BoundBuffer* boundBuffer = SparseArray64_get(&textureDecoder->boundBuffers, (int64_t)srcBuffer);
    if (!boundBuffer) return;

    TextureDecoder_Image* targetImage = SparseArray64_get(&textureDecoder->images, (int64_t)dstImage);
    if (!targetImage) return;

    for (int i = 0; i < regionCount; i++) recordCopy(textureDecoder, commandBuffer, boundBuffer, targetImage, dstImageLayout, &regions[i]);
}

void TextureDecoder_copyBufferToImage2(TextureDecoder* textureDecoder, VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage, VkImageLayout dstImageLayout, VkCopyBufferToImageInfo2* copyBufferToImageInfo) {
    TextureDecoder_BoundBuffer* boundBuffer = SparseArray64_get(&textureDecoder->boundBuffers, (int64_t)srcBuffer);
    if (!boundBuffer) return;

    TextureDecoder_Image* targetImage = SparseArray64_get(&textureDecoder->images, (int64_t)dstImage);
    if (!targetImage) return;

    for (int i = 0; i < copyBufferToImageInfo->regionCount; i++) {
        VkBufferImageCopy region;
        const char* src = ((const char*)&copyBufferToImageInfo->pRegions[i]) + offsetof(VkBufferImageCopy2, bufferOffset);
        memcpy(&region, src, sizeof(VkBufferImageCopy));
        recordCopy(textureDecoder, commandBuffer, boundBuffer, targetImage, dstImageLayout, &region);
    }
}

void TextureDecoder_destroyDetached(TextureDecoder* textureDecoder) {
    // The client disappeared without vkDestroyDevice: game command buffers that read our staging
    // memory may still be executing. Destroy only once the queue is idle; otherwise leave the
    // Vulkan objects to the (leaked) device rather than freeing memory the GPU may be reading.
    bool idle = false;
    if (textureDecoder->queue) {
        VkFenceCreateInfo fenceInfo = {0};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        if (vulkanWrapper.vkCreateFence(textureDecoder->device, &fenceInfo, NULL, &fence) == VK_SUCCESS) {
            if (vulkanWrapper.vkQueueSubmit(textureDecoder->queue, 0, NULL, fence) == VK_SUCCESS) {
                idle = vulkanWrapper.vkWaitForFences(textureDecoder->device, 1, &fence, VK_TRUE, 2000000000ull) == VK_SUCCESS;
            }
            vulkanWrapper.vkDestroyFence(textureDecoder->device, fence, NULL);
        }
    }

    if (idle) TextureDecoder_destroy(textureDecoder);
    else println("vortek: texture decoder: GPU queue not idle at disconnect, staging memory left to the device");
}

bool TextureDecoder_containsImage(TextureDecoder* textureDecoder, VkImage image) {
    return SparseArray64_indexOfKey(&textureDecoder->images, (int64_t)image) >= 0;
}

static void mapFormatList(VkImageCreateInfo* imageInfo) {
    VkImageFormatListCreateInfo* formatListInfo = findNextVkStructure((void*)imageInfo->pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO);
    if (!formatListInfo || !formatListInfo->pViewFormats || formatListInfo->viewFormatCount == 0) return;

    VkFormat* viewFormats = (VkFormat*)formatListInfo->pViewFormats;
    uint32_t count = 0;
    for (uint32_t i = 0; i < formatListInfo->viewFormatCount; i++) {
        if (!TextureDecoder_canDecodeFormat(viewFormats[i])) continue;
        VkFormat mapped = TextureDecoder_getDecompressedFormat(viewFormats[i]);
        bool duplicate = false;
        for (uint32_t j = 0; j < count && !duplicate; j++) duplicate = viewFormats[j] == mapped;
        if (!duplicate) viewFormats[count++] = mapped;
    }

    if (count == 0) viewFormats[count++] = imageInfo->format;
    formatListInfo->viewFormatCount = count;
}

VkResult TextureDecoder_createImage(TextureDecoder* textureDecoder, VkImageCreateInfo* imageInfo, VkImage* pImage) {
    *pImage = VK_NULL_HANDLE;
    VkResult result;
    // vkCreateImage cannot return VK_ERROR_FORMAT_NOT_SUPPORTED; callers only get here for decodable formats.
    if (!TextureDecoder_canDecodeFormat(imageInfo->format)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    TextureDecoder_Image* newImage = calloc(1, sizeof(TextureDecoder_Image));
    newImage->format = imageInfo->format;
    newImage->width = imageInfo->extent.width;
    newImage->height = imageInfo->extent.height;

    imageInfo->format = TextureDecoder_getDecompressedFormat(newImage->format);
    imageInfo->flags &= ~(VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT);
    mapFormatList(imageInfo);

    VkImage image;
    result = vulkanWrapper.vkCreateImage(textureDecoder->device, imageInfo, NULL, &image);
    if (result != VK_SUCCESS) goto error;

    newImage->image = image;
    SparseArray64_put(&textureDecoder->images, (int64_t)image, newImage);

    *pImage = image;
    return result;

error:
    internalDestroyImage(textureDecoder->device, newImage);
    return result;
}

/* Pending copies keep raw pointers: invalidate them before the target object is freed. */
static void invalidatePendingCopies(TextureDecoder* textureDecoder, TextureDecoder_Image* image, TextureDecoder_BoundBuffer* buffer) {
    ArrayDeque* deque = &textureDecoder->bufferImageCopies;
    if (deque->elements) {
        for (int i = deque->head; i != deque->tail; i = (i + 1) & (deque->size - 1)) {
            TextureDecoder_BufferImageCopy* copy = deque->elements[i];
            if (!copy) continue;
            if (image && copy->dstImage == image) copy->dstImage = NULL;
            if (buffer && copy->srcBuffer == buffer) copy->srcBuffer = NULL;
        }
    }

    for (int i = 0; i < textureDecoder->commandBuffers.size; i++) {
        TextureDecoder_CommandBufferState* state = textureDecoder->commandBuffers.entries[i].value;
        if (!state) continue;
        for (int j = 0; j < state->jobs.size; j++) {
            TextureDecoder_InlineJob* job = state->jobs.elements[j];
            if (image && job->dstImage == image) job->dstImage = NULL;
            if (buffer && job->srcBuffer == buffer) job->srcBuffer = NULL;
        }
    }
}

void TextureDecoder_destroyImage(TextureDecoder* textureDecoder, VkImage image) {
    TextureDecoder_Image* targetImage = SparseArray64_get(&textureDecoder->images, (int64_t)image);
    if (targetImage) {
        invalidatePendingCopies(textureDecoder, targetImage, NULL);
        SparseArray64_remove(&textureDecoder->images, (int64_t)image);
        internalDestroyImage(textureDecoder->device, targetImage);
    }
}

void TextureDecoder_addBoundBuffer(TextureDecoder* textureDecoder, ResourceMemory* memory, VkBuffer buffer, VkDeviceSize memoryOffset) {
    if (memory->fd <= 0) return;
    TextureDecoder_removeBoundBuffer(textureDecoder, buffer);

    TextureDecoder_BoundBuffer* boundBuffer = calloc(1, sizeof(TextureDecoder_BoundBuffer));
    boundBuffer->buffer = buffer;
    boundBuffer->memoryOffset = memoryOffset;
    boundBuffer->memory = memory;

    SparseArray64_put(&textureDecoder->boundBuffers, (int64_t)buffer, boundBuffer);
}

void TextureDecoder_removeBoundBuffer(TextureDecoder* textureDecoder, VkBuffer buffer) {
    TextureDecoder_BoundBuffer* boundBuffer = SparseArray64_get(&textureDecoder->boundBuffers, (int64_t)buffer);
    if (boundBuffer) {
        invalidatePendingCopies(textureDecoder, NULL, boundBuffer);
        SparseArray64_remove(&textureDecoder->boundBuffers, (int64_t)buffer);
        MEMFREE(boundBuffer);
    }
}

bool TextureDecoder_isFormatSupported(TextureDecoder* textureDecoder, VkFormat format) {
    // Only formats the decoder can actually decode. ETC2/EAC/ASTC (native on Mali) pass straight through.
    if (!TextureDecoder_canDecodeFormat(format)) return false;
    if (textureDecoder->deviceSupportedFormats > 0 && (textureDecoder->deviceSupportedFormats & VK_FORMAT_BC_FLAG(format))) return false;
    return true;
}

bool isCompressedFormat(VkFormat format) {
    switch(format) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11_SNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
            return true;
        default:
            return false;
    }
}
