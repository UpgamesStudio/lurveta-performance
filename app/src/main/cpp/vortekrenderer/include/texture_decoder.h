#ifndef VORTEK_TEXTURE_DECODER_H
#define VORTEK_TEXTURE_DECODER_H

#include "vortek.h"

/*
 * LURVETA Performance — BC texture decoder (used only when the GPU lacks textureCompressionBC,
 * e.g. ARM Mali). Decoded texels are staged in a small ring of host-visible buffers and uploaded
 * with ONE command buffer / ONE vkQueueSubmit per game submit, instead of one submit plus a
 * blocking vkWaitForFences per copied region.
 */

#define TEXTURE_DECODER_STAGING_SLOTS 2
#define TEXTURE_DECODER_MIN_STAGING_SIZE (32u << 20)

typedef struct TextureDecoder_Image {
    VkImage image;
    VkFormat format;
    short width;
    short height;
} TextureDecoder_Image;

typedef struct TextureDecoder_BoundBuffer {
    VkBuffer buffer;
    VkDeviceSize memoryOffset;
    ResourceMemory* memory;
} TextureDecoder_BoundBuffer;

typedef struct TextureDecoder_BufferImageCopy {
    TextureDecoder_BoundBuffer* srcBuffer;
    TextureDecoder_Image* dstImage;
    VkImageLayout dstImageLayout;
    VkBufferImageCopy region;
} TextureDecoder_BufferImageCopy;

typedef struct TextureDecoder_StagingSlot {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize capacity;
    VkDeviceSize used;
    uint8_t* mapped;
    bool coherent;
    VkCommandBuffer commandBuffer;
    VkFence fence;
    bool recording;
    bool pending;
} TextureDecoder_StagingSlot;

typedef struct TextureDecoder {
    SparseArray64 images;
    SparseArray64 boundBuffers;
    ArrayDeque bufferImageCopies;
    short imageCacheSize;
    ThreadPool* threadPool;
    uint32_t deviceSupportedFormats;

    VkDevice device;
    VkQueue queue;
    VkCommandPool commandPool;

    TextureDecoder_StagingSlot slots[TEXTURE_DECODER_STAGING_SLOTS];
    int currentSlot;

    bool cacheSizeLoaded;
    uint64_t cacheSizeBytes;

    /* Counters (diagnostics). */
    uint64_t statRegions;
    uint64_t statCacheHits;
    uint64_t statSubmits;
    uint64_t statSlotWaits;
} TextureDecoder;

extern TextureDecoder* TextureDecoder_create(VkContext* context, VkPhysicalDevice physicalDevice, VkDevice device, VkPhysicalDeviceFeatures* supportedFeatures);
extern void TextureDecoder_destroy(TextureDecoder* textureDecoder);
/* Decodes and uploads all pending regions before a game submit on submitQueue. */
extern void TextureDecoder_decodeAll(TextureDecoder* textureDecoder, VkQueue submitQueue);
extern void TextureDecoder_copyBufferToImage(TextureDecoder* textureDecoder, VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, VkBufferImageCopy* regions);
extern void TextureDecoder_copyBufferToImage2(TextureDecoder* textureDecoder, VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage, VkImageLayout dstImageLayout, VkCopyBufferToImageInfo2* copyBufferToImageInfo);
extern bool TextureDecoder_containsImage(TextureDecoder* textureDecoder, VkImage image);
extern VkResult TextureDecoder_createImage(TextureDecoder* textureDecoder, VkImageCreateInfo* imageInfo, VkImage* pImage);
extern void TextureDecoder_destroyImage(TextureDecoder* textureDecoder, VkImage image);
extern void TextureDecoder_addBoundBuffer(TextureDecoder* textureDecoder, ResourceMemory* resourceMemory, VkBuffer buffer, VkDeviceSize memoryOffset);
extern void TextureDecoder_removeBoundBuffer(TextureDecoder* textureDecoder, VkBuffer buffer);
extern bool TextureDecoder_isFormatSupported(TextureDecoder* textureDecoder, VkFormat format);
extern bool isCompressedFormat(VkFormat format);
extern VkResult getCompressedImageFormatProperties(VkFormat format, VkImageFormatProperties* pImageFormatProperties);

/*
 * Uncompressed format used to store a decoded BC format:
 *   BC1/BC2/BC3 UNORM -> B8G8R8A8_UNORM, BC1/BC2/BC3 SRGB -> B8G8R8A8_SRGB (sampling linearizes),
 *   BC4 -> R8_UNORM / R8_SNORM, BC5 -> R8G8_UNORM / R8G8_SNORM.
 * Other compressed formats keep the legacy B8G8R8A8_UNORM mapping.
 */
extern VkFormat TextureDecoder_getDecompressedFormat(VkFormat format);
extern uint32_t TextureDecoder_getDecompressedTexelSize(VkFormat compressedFormat);

#endif
