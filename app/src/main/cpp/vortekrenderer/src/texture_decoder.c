#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "texture_decoder.h"
#include "vulkan_helper.h"
#include "bc_decoder.h"
#include "string_utils.h"
#include "file_utils.h"

#define CACHE_DIR APP_CACHE_DIR "/vortek"
#define CACHE_MIN_IMAGE_WIDTH 1024

/* LURVETA: see texture_decoder.h for the upload/caching design. */

#define VK_FORMAT_BC_FLAG(f) (1 << (f - VK_FORMAT_BC1_RGB_UNORM_BLOCK))

static bool isCanDecompressFormat(VkFormat format) {
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
        default:
            *blockSize = 0;
            *bcN = 0;
            break;
    }
}

static uint32_t getDeviceSupportedFormats(VkPhysicalDevice physicalDevice) {
    const VkFormat formats[] = {
        VK_FORMAT_BC1_RGB_UNORM_BLOCK,
        VK_FORMAT_BC1_RGB_SRGB_BLOCK,
        VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
        VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
        VK_FORMAT_BC2_UNORM_BLOCK,
        VK_FORMAT_BC2_SRGB_BLOCK,
        VK_FORMAT_BC3_UNORM_BLOCK,
        VK_FORMAT_BC3_SRGB_BLOCK,
        VK_FORMAT_BC4_UNORM_BLOCK,
        VK_FORMAT_BC4_SNORM_BLOCK,
        VK_FORMAT_BC5_UNORM_BLOCK,
        VK_FORMAT_BC5_SNORM_BLOCK
    };

    uint32_t flags = 0;
    for (int i = 0; i < ARRAY_SIZE(formats); i++) {
        VkFormatProperties formatProperties = {0};
        vulkanWrapper.vkGetPhysicalDeviceFormatProperties(physicalDevice, formats[i], &formatProperties);
        if (formatProperties.optimalTilingFeatures > 0) flags |= VK_FORMAT_BC_FLAG(formats[i]);
    }
    return flags;
}

static void internalDestroyImage(VkDevice device, TextureDecoder_Image* targetImage) {
    if (targetImage->image) vulkanWrapper.vkDestroyImage(device, targetImage->image, NULL);
    MEMFREE(targetImage);
}

VkFormat TextureDecoder_getDecompressedFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
            return VK_FORMAT_B8G8R8A8_SRGB;
        case VK_FORMAT_BC4_UNORM_BLOCK:
            return VK_FORMAT_R8_UNORM;
        case VK_FORMAT_BC4_SNORM_BLOCK:
            return VK_FORMAT_R8_SNORM;
        case VK_FORMAT_BC5_UNORM_BLOCK:
            return VK_FORMAT_R8G8_UNORM;
        case VK_FORMAT_BC5_SNORM_BLOCK:
            return VK_FORMAT_R8G8_SNORM;
        default:
            return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

uint32_t TextureDecoder_getDecompressedTexelSize(VkFormat compressedFormat) {
    int blockSize, bcN;
    bool isNoAlphaU;
    getBCInfo(compressedFormat, &bcN, &blockSize, &isNoAlphaU);
    return bcN > 0 ? (uint32_t)BCDecoder_getOutputTexelSize(bcN) : 4;
}

/* ---------------------------------------------------------------------------------------------
 * Persistent cache of decoded images (only for regions >= CACHE_MIN_IMAGE_WIDTH in both axes).
 * LURVETA fixes: the file size is the size of the decoded REGION (not of mip 0), the key contains
 * the region extent, the destination buffer is always large enough before reading, and the total
 * cache size is kept in memory instead of re-reading ".cache-size" for every write.
 * ------------------------------------------------------------------------------------------- */
static void getCacheFilename(char* filename, uint64_t hash, uint32_t width, uint32_t height, VkFormat format) {
    sprintf(filename, CACHE_DIR "/%lx-%ux%u-%d.imd", (unsigned long)hash, width, height, format);
}

static bool readCachedRegion(uint64_t hash, uint32_t width, uint32_t height, VkFormat format, void* result, size_t size) {
    char filename[160] = {0};
    getCacheFilename(filename, hash, width, height, format);
    size_t expected = size;
    return fileGetContents(filename, result, &expected) ? true : false;
}

static void loadCacheSize(TextureDecoder* textureDecoder) {
    if (textureDecoder->cacheSizeLoaded) return;
    createDirectory(CACHE_DIR);
    char* content = fileGetContents(CACHE_DIR "/.cache-size", NULL, NULL);
    textureDecoder->cacheSizeBytes = 0;
    if (content) {
        textureDecoder->cacheSizeBytes = strtoull(content, NULL, 10);
        MEMFREE(content);
    }
    textureDecoder->cacheSizeLoaded = true;
}

static void writeCachedRegion(TextureDecoder* textureDecoder, uint64_t hash, uint32_t width, uint32_t height, VkFormat format, const void* data, size_t size) {
    if (textureDecoder->imageCacheSize <= 0) return;
    loadCacheSize(textureDecoder);

    uint64_t maxCacheSize = (uint64_t)textureDecoder->imageCacheSize << 20;
    if (size > maxCacheSize) return;
    while (textureDecoder->cacheSizeBytes + size > maxCacheSize) {
        FindFileInfo fileInfo = {0};
        if (findFirstFile(CACHE_DIR, &fileInfo) && remove(fileInfo.path) == 0) {
            textureDecoder->cacheSizeBytes = textureDecoder->cacheSizeBytes > fileInfo.size ? textureDecoder->cacheSizeBytes - fileInfo.size : 0;
        }
        else {
            textureDecoder->cacheSizeBytes = 0;
            break;
        }
    }

    char filename[160] = {0};
    getCacheFilename(filename, hash, width, height, format);
    if (filePutContents(filename, (void*)data, size)) {
        textureDecoder->cacheSizeBytes += size;
        char value[32] = {0};
        sprintf(value, "%llu", (unsigned long long)textureDecoder->cacheSizeBytes);
        filePutContents(CACHE_DIR "/.cache-size", value, strlen(value));
    }
    else remove(filename);
}

static uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) {
    for (uint32_t i = 0; i < 32; i++) {
        if ((typeBits & (1u << i)) && (getMemoryPropertyFlags(i) & required) == required) return i;
    }
    return UINT32_MAX;
}

static void destroyStagingBuffer(TextureDecoder* textureDecoder, TextureDecoder_StagingSlot* slot) {
    if (slot->mapped) {
        vulkanWrapper.vkUnmapMemory(textureDecoder->device, slot->memory);
        slot->mapped = NULL;
    }
    if (slot->buffer) {
        vulkanWrapper.vkDestroyBuffer(textureDecoder->device, slot->buffer, NULL);
        slot->buffer = NULL;
    }
    if (slot->memory) {
        vulkanWrapper.vkFreeMemory(textureDecoder->device, slot->memory, NULL);
        slot->memory = NULL;
    }
    slot->capacity = 0;
    slot->used = 0;
}

static bool createStagingBuffer(TextureDecoder* textureDecoder, TextureDecoder_StagingSlot* slot, VkDeviceSize size) {
    destroyStagingBuffer(textureDecoder, slot);

    VkBufferCreateInfo bufferInfo = {0};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VkResult result = vulkanWrapper.vkCreateBuffer(textureDecoder->device, &bufferInfo, NULL, &slot->buffer);
    if (result != VK_SUCCESS) goto error;

    VkMemoryRequirements memReqs = {0};
    vulkanWrapper.vkGetBufferMemoryRequirements(textureDecoder->device, slot->buffer, &memReqs);

    uint32_t memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    slot->coherent = memoryTypeIndex != UINT32_MAX;
    if (!slot->coherent) memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (memoryTypeIndex == UINT32_MAX) memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

    VkMemoryAllocateInfo allocateInfo = {0};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = memReqs.size;
    allocateInfo.memoryTypeIndex = memoryTypeIndex;

    result = vulkanWrapper.vkAllocateMemory(textureDecoder->device, &allocateInfo, NULL, &slot->memory);
    if (result != VK_SUCCESS) goto error;

    result = vulkanWrapper.vkBindBufferMemory(textureDecoder->device, slot->buffer, slot->memory, 0);
    if (result != VK_SUCCESS) goto error;

    void* mapped = NULL;
    result = vulkanWrapper.vkMapMemory(textureDecoder->device, slot->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result != VK_SUCCESS) goto error;

    slot->mapped = mapped;
    slot->capacity = size;
    slot->used = 0;
    return true;

error:
    println("vortek: texture decoder: unable to create a staging buffer of %llu bytes", (unsigned long long)size);
    destroyStagingBuffer(textureDecoder, slot);
    return false;
}

static void waitSlot(TextureDecoder* textureDecoder, TextureDecoder_StagingSlot* slot) {
    if (!slot->pending) return;
    if (vulkanWrapper.vkGetFenceStatus(textureDecoder->device, slot->fence) != VK_SUCCESS) {
        vulkanWrapper.vkWaitForFences(textureDecoder->device, 1, &slot->fence, VK_TRUE, UINT64_MAX);
        textureDecoder->statSlotWaits++;
    }
    vulkanWrapper.vkResetFences(textureDecoder->device, 1, &slot->fence);
    slot->pending = false;
}

static void submitSlot(TextureDecoder* textureDecoder, TextureDecoder_StagingSlot* slot) {
    if (!slot->recording) return;

    // Make the uploads available/visible to every command submitted later on this queue.
    VkMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vulkanWrapper.vkCmdPipelineBarrier(slot->commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    vulkanWrapper.vkEndCommandBuffer(slot->commandBuffer);

    if (!slot->coherent) {
        VkMappedMemoryRange range = {0};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = slot->memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vulkanWrapper.vkFlushMappedMemoryRanges(textureDecoder->device, 1, &range);
    }

    VkSubmitInfo submitInfo = {0};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &slot->commandBuffer;

    vulkanWrapper.vkQueueSubmit(textureDecoder->queue, 1, &submitInfo, slot->fence);
    slot->recording = false;
    slot->pending = true;
    textureDecoder->statSubmits++;
}

/* Returns the current slot with at least `size` free bytes (offset aligned), recording. */
static TextureDecoder_StagingSlot* acquireStaging(TextureDecoder* textureDecoder, VkDeviceSize size, VkDeviceSize* outOffset) {
    TextureDecoder_StagingSlot* slot = &textureDecoder->slots[textureDecoder->currentSlot];
    VkDeviceSize offset = (slot->used + 255) & ~(VkDeviceSize)255;

    if (slot->recording && offset + size > slot->capacity) {
        submitSlot(textureDecoder, slot);
        textureDecoder->currentSlot = (textureDecoder->currentSlot + 1) % TEXTURE_DECODER_STAGING_SLOTS;
        slot = &textureDecoder->slots[textureDecoder->currentSlot];
        offset = 0;
    }

    if (!slot->recording) {
        waitSlot(textureDecoder, slot);
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
        if (slot->pending && slot->fence) vulkanWrapper.vkWaitForFences(textureDecoder->device, 1, &slot->fence, VK_TRUE, UINT64_MAX);
        slot->pending = false;
        destroyStagingBuffer(textureDecoder, slot);
        if (slot->commandBuffer) {
            vulkanWrapper.vkFreeCommandBuffers(textureDecoder->device, textureDecoder->commandPool, 1, &slot->commandBuffer);
            slot->commandBuffer = NULL;
        }
        if (slot->fence) {
            vulkanWrapper.vkDestroyFence(textureDecoder->device, slot->fence, NULL);
            slot->fence = NULL;
        }
    }
    if (textureDecoder->commandPool) {
        vulkanWrapper.vkDestroyCommandPool(textureDecoder->device, textureDecoder->commandPool, NULL);
        textureDecoder->commandPool = NULL;
    }
}

static bool createSlots(TextureDecoder* textureDecoder, uint32_t graphicsQueueIndex) {
    VkCommandPoolCreateInfo commandPoolInfo = {0};
    commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    commandPoolInfo.queueFamilyIndex = graphicsQueueIndex;

    VkResult result = vulkanWrapper.vkCreateCommandPool(textureDecoder->device, &commandPoolInfo, NULL, &textureDecoder->commandPool);
    if (result != VK_SUCCESS) goto error;

    for (int i = 0; i < TEXTURE_DECODER_STAGING_SLOTS; i++) {
        TextureDecoder_StagingSlot* slot = &textureDecoder->slots[i];

        VkCommandBufferAllocateInfo commandBufferInfo = {0};
        commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandBufferInfo.commandPool = textureDecoder->commandPool;
        commandBufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandBufferInfo.commandBufferCount = 1;

        result = vulkanWrapper.vkAllocateCommandBuffers(textureDecoder->device, &commandBufferInfo, &slot->commandBuffer);
        if (result != VK_SUCCESS) goto error;

        VkFenceCreateInfo fenceInfo = {0};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        result = vulkanWrapper.vkCreateFence(textureDecoder->device, &fenceInfo, NULL, &slot->fence);
        if (result != VK_SUCCESS) goto error;
    }

    vulkanWrapper.vkGetDeviceQueue(textureDecoder->device, graphicsQueueIndex, 0, &textureDecoder->queue);
    return true;

error:
    destroySlots(textureDecoder);
    return false;
}

TextureDecoder* TextureDecoder_create(VkContext* context, VkPhysicalDevice physicalDevice, VkDevice device, VkPhysicalDeviceFeatures* supportedFeatures) {
    if (supportedFeatures->textureCompressionBC) return NULL;
    TextureDecoder* textureDecoder = calloc(1, sizeof(TextureDecoder));
    textureDecoder->device = device;
    textureDecoder->imageCacheSize = context->imageCacheSize;
    textureDecoder->threadPool = context->threadPool;

    createSlots(textureDecoder, context->graphicsQueueIndex);
    if (context->driverID == VK_DRIVER_ID_SAMSUNG_PROPRIETARY) textureDecoder->deviceSupportedFormats = getDeviceSupportedFormats(physicalDevice);
    return textureDecoder;
}

void TextureDecoder_destroy(TextureDecoder* textureDecoder) {
    destroySlots(textureDecoder);

    SparseArray64_free(&textureDecoder->images, true);
    SparseArray64_free(&textureDecoder->boundBuffers, true);
    ArrayDeque_free(&textureDecoder->bufferImageCopies, true);
    MEMFREE(textureDecoder);
}

void TextureDecoder_decodeAll(TextureDecoder* textureDecoder, VkQueue submitQueue) {
    if (!textureDecoder->commandPool) {
        // Initialization failed: drop the pending copies instead of touching invalid objects.
        TextureDecoder_BufferImageCopy* pending;
        while (!ArrayDeque_isEmpty(&textureDecoder->bufferImageCopies) && (pending = ArrayDeque_removeFirst(&textureDecoder->bufferImageCopies))) MEMFREE(pending);
        return;
    }

    bool submitted = false;
    while (!ArrayDeque_isEmpty(&textureDecoder->bufferImageCopies)) {
        TextureDecoder_BufferImageCopy* bufferImageCopy = ArrayDeque_removeFirst(&textureDecoder->bufferImageCopies);
        if (!bufferImageCopy) break;

        TextureDecoder_BoundBuffer* srcBuffer = bufferImageCopy->srcBuffer;
        TextureDecoder_Image* dstImage = bufferImageCopy->dstImage;
        VkBufferImageCopy region = bufferImageCopy->region;
        VkImageLayout dstImageLayout = bufferImageCopy->dstImageLayout;
        MEMFREE(bufferImageCopy);
        if (!srcBuffer || !dstImage || !srcBuffer->memory) continue;

        int blockSize, bcN;
        bool isNoAlphaU;
        getBCInfo(dstImage->format, &bcN, &blockSize, &isNoAlphaU);
        if (bcN == 0) continue;

        const uint32_t width = region.imageExtent.width;
        const uint32_t height = region.imageExtent.height;
        const uint32_t slices = MAX(region.imageExtent.depth, 1) * MAX(region.imageSubresource.layerCount, 1);
        if (width == 0 || height == 0) continue;

        const uint32_t texelSize = BCDecoder_getOutputTexelSize(bcN);
        const VkDeviceSize compressedSliceSize = (VkDeviceSize)((width + 3) / 4) * ((height + 3) / 4) * blockSize;
        const VkDeviceSize decodedSliceSize = (VkDeviceSize)width * height * texelSize;
        const VkDeviceSize totalSize = decodedSliceSize * slices;

        VkDeviceSize stagingOffset = 0;
        TextureDecoder_StagingSlot* slot = acquireStaging(textureDecoder, totalSize, &stagingOffset);
        if (!slot) continue;

        VkDeviceSize sourceOffset = srcBuffer->memoryOffset + region.bufferOffset;
        if (sourceOffset + compressedSliceSize * slices > srcBuffer->memory->allocationSize) continue;

        uint8_t* bufferData = mmap(NULL, srcBuffer->memory->allocationSize, PROT_READ, MAP_SHARED, srcBuffer->memory->fd, 0);
        if (bufferData == MAP_FAILED) continue;

        const uint8_t* imageData = bufferData + sourceOffset;
        uint8_t* decoded = slot->mapped + stagingOffset;
        const bool cacheable = width >= CACHE_MIN_IMAGE_WIDTH && height >= CACHE_MIN_IMAGE_WIDTH;

        for (uint32_t s = 0; s < slices; s++) {
            const uint8_t* src = imageData + s * compressedSliceSize;
            uint8_t* dst = decoded + s * decodedSliceSize;

            if (cacheable) {
                uint64_t hash = murmurHash64((const char*)src, compressedSliceSize, dstImage->format);
                if (readCachedRegion(hash, width, height, dstImage->format, dst, decodedSliceSize)) {
                    textureDecoder->statCacheHits++;
                    continue;
                }
                BCDecoder_decode(src, dst, width, height, bcN, isNoAlphaU, textureDecoder->threadPool);
                writeCachedRegion(textureDecoder, hash, width, height, dstImage->format, dst, decodedSliceSize);
            }
            else BCDecoder_decode(src, dst, width, height, bcN, isNoAlphaU, textureDecoder->threadPool);
        }

        munmap(bufferData, srcBuffer->memory->allocationSize);

        region.bufferOffset = stagingOffset;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        vulkanWrapper.vkCmdCopyBufferToImage(slot->commandBuffer, slot->buffer, dstImage->image, dstImageLayout, 1, &region);
        slot->used = stagingOffset + totalSize;
        textureDecoder->statRegions++;
        submitted = true;
    }

    if (!submitted) return;
    TextureDecoder_StagingSlot* slot = &textureDecoder->slots[textureDecoder->currentSlot];
    submitSlot(textureDecoder, slot);

    // Submission order + the memory barrier guarantee that the uploads complete before the game's
    // commands only on the SAME queue. Otherwise keep the old blocking behavior for correctness.
    if (submitQueue != textureDecoder->queue) {
        for (int i = 0; i < TEXTURE_DECODER_STAGING_SLOTS; i++) waitSlot(textureDecoder, &textureDecoder->slots[i]);
    }
    textureDecoder->currentSlot = (textureDecoder->currentSlot + 1) % TEXTURE_DECODER_STAGING_SLOTS;
}

void TextureDecoder_copyBufferToImage(TextureDecoder* textureDecoder, VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, VkBufferImageCopy* regions) {
    TextureDecoder_BoundBuffer* boundBuffer = SparseArray64_get(&textureDecoder->boundBuffers, (int64_t)srcBuffer);
    if (!boundBuffer) return;

    TextureDecoder_Image* targetImage = SparseArray64_get(&textureDecoder->images, (int64_t)dstImage);
    if (!targetImage) return;

    for (int i = 0; i < regionCount; i++) {
        TextureDecoder_BufferImageCopy* bufferImageCopy = calloc(1, sizeof(TextureDecoder_BufferImageCopy));
        bufferImageCopy->srcBuffer = boundBuffer;
        bufferImageCopy->dstImage = targetImage;
        bufferImageCopy->dstImageLayout = dstImageLayout;
        memcpy(&bufferImageCopy->region, &regions[i], sizeof(VkBufferImageCopy));
        ArrayDeque_addLast(&textureDecoder->bufferImageCopies, bufferImageCopy);
    }
}

void TextureDecoder_copyBufferToImage2(TextureDecoder* textureDecoder, VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage, VkImageLayout dstImageLayout, VkCopyBufferToImageInfo2* copyBufferToImageInfo) {
    TextureDecoder_BoundBuffer* boundBuffer = SparseArray64_get(&textureDecoder->boundBuffers, (int64_t)srcBuffer);
    if (!boundBuffer) return;

    TextureDecoder_Image* targetImage = SparseArray64_get(&textureDecoder->images, (int64_t)dstImage);
    if (!targetImage) return;

    for (int i = 0; i < copyBufferToImageInfo->regionCount; i++) {
        TextureDecoder_BufferImageCopy* bufferImageCopy = calloc(1, sizeof(TextureDecoder_BufferImageCopy));
        bufferImageCopy->srcBuffer = boundBuffer;
        bufferImageCopy->dstImage = targetImage;
        bufferImageCopy->dstImageLayout = dstImageLayout;

        const char* region = ((char*)&copyBufferToImageInfo->pRegions[i]) + offsetof(VkBufferImageCopy2, bufferOffset);
        memcpy(&bufferImageCopy->region, region, sizeof(VkBufferImageCopy));
        ArrayDeque_addLast(&textureDecoder->bufferImageCopies, bufferImageCopy);
    }
}

bool TextureDecoder_containsImage(TextureDecoder* textureDecoder, VkImage image) {
    return SparseArray64_indexOfKey(&textureDecoder->images, (int64_t)image) >= 0;
}

static void mapFormatList(VkImageCreateInfo* imageInfo) {
    VkImageFormatListCreateInfo* formatListInfo = findNextVkStructure(imageInfo->pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO);
    if (!formatListInfo || !formatListInfo->pViewFormats || formatListInfo->viewFormatCount == 0) return;

    VkFormat* viewFormats = (VkFormat*)formatListInfo->pViewFormats;
    uint32_t count = 0;
    for (uint32_t i = 0; i < formatListInfo->viewFormatCount; i++) {
        if (!isCanDecompressFormat(viewFormats[i])) continue;
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
    if (!isCanDecompressFormat(imageInfo->format)) return VK_ERROR_FORMAT_NOT_SUPPORTED;

    TextureDecoder_Image* newImage = calloc(1, sizeof(TextureDecoder_Image));
    newImage->format = imageInfo->format;
    newImage->width = imageInfo->extent.width;
    newImage->height = imageInfo->extent.height;

    imageInfo->format = TextureDecoder_getDecompressedFormat(newImage->format);
    imageInfo->flags &= ~VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
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
    if (!deque->elements) return;
    for (int i = deque->head; i != deque->tail; i = (i + 1) & (deque->size - 1)) {
        TextureDecoder_BufferImageCopy* copy = deque->elements[i];
        if (!copy) continue;
        if (image && copy->dstImage == image) copy->dstImage = NULL;
        if (buffer && copy->srcBuffer == buffer) copy->srcBuffer = NULL;
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
    if (!isCompressedFormat(format)) return false;
    if (textureDecoder->deviceSupportedFormats > 0) {
        bool result = textureDecoder->deviceSupportedFormats & VK_FORMAT_BC_FLAG(format);
        if (result) return false;
    }
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

VkResult getCompressedImageFormatProperties(VkFormat format, VkImageFormatProperties* pImageFormatProperties) {
    if (isCanDecompressFormat(format)) {
        pImageFormatProperties->maxExtent.width = 16384;
        pImageFormatProperties->maxExtent.height = 16384;
        pImageFormatProperties->maxExtent.depth = 1;
        pImageFormatProperties->maxMipLevels = 15;
        pImageFormatProperties->maxArrayLayers = 2048;
        pImageFormatProperties->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
        pImageFormatProperties->maxResourceSize = 1u << 31;
        return VK_SUCCESS;
    }
    else return VK_ERROR_FORMAT_NOT_SUPPORTED;
}