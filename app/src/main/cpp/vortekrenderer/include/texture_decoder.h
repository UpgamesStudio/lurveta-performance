#ifndef VORTEK_TEXTURE_DECODER_H
#define VORTEK_TEXTURE_DECODER_H

#include "vortek.h"

/*
 * LURVETA Performance — software decoder for block-compressed textures the GPU cannot sample
 * (BC1-BC7 on ARM Mali). Native formats (ETC2, EAC, ASTC, and BC on GPUs that support it) are never
 * touched.
 *
 * Upload modes (Vortek setting "textureUploadMode"):
 *  - INLINE (default): the decoded copy is recorded INTO THE GAME'S COMMAND BUFFER, at the exact
 *    position of the game's vkCmdCopyBufferToImage, reading from a staging region owned by that
 *    command buffer. Decoding happens when the command buffer is submitted. The game's own
 *    barriers, layout transitions and queue ownership transfers therefore apply unchanged: no
 *    hoisting, no write-after-write between copies, no CPU wait on the GPU. Staging memory is
 *    recycled when the command buffer is reset/re-begun/freed (Vulkan forbids that while pending).
 *  - BATCHED_SAFE: LURVETA 1.0.0 batching (copies hoisted before the game's submit, one submit per
 *    batch) plus a transfer barrier whenever a batch writes the same image twice.
 *  - BATCHED: LURVETA 1.0.0 batching, unchanged (kept for A/B comparison).
 *  - SYNC: Winlator 11.2 behaviour (one submit + one blocking wait per region; kept for A/B).
 */

#define TEXTURE_UPLOAD_MODE_INLINE 0
#define TEXTURE_UPLOAD_MODE_BATCHED_SAFE 1
#define TEXTURE_UPLOAD_MODE_BATCHED 2
#define TEXTURE_UPLOAD_MODE_SYNC 3

#define TEXTURE_DECODER_STAGING_SLOTS 2
#define TEXTURE_DECODER_MIN_STAGING_SIZE (32u << 20)
#define TEXTURE_DECODER_CHUNK_SIZE (4u << 20)
#define TEXTURE_DECODER_MAX_FREE_CHUNK_BYTES (96u << 20)

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

/* INLINE mode: host-visible staging memory owned by one recorded command buffer. */
typedef struct TextureDecoder_StagingChunk {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t* mapped;
    VkDeviceSize capacity;
    VkDeviceSize used;
    bool coherent;
} TextureDecoder_StagingChunk;

/* INLINE mode: one copy region waiting to be decoded when its command buffer is submitted. */
typedef struct TextureDecoder_InlineJob {
    TextureDecoder_BoundBuffer* srcBuffer;
    TextureDecoder_Image* dstImage;
    VkBufferImageCopy srcRegion;
    TextureDecoder_StagingChunk* chunk;
    VkDeviceSize stagingOffset;
} TextureDecoder_InlineJob;

typedef struct TextureDecoder_CommandBufferState {
    VkCommandBuffer commandBuffer;
    VkCommandPool commandPool;
    ArrayList jobs;        /* TextureDecoder_InlineJob* */
    ArrayList chunks;      /* TextureDecoder_StagingChunk* */
    ArrayList secondaries; /* VkCommandBuffer executed through vkCmdExecuteCommands */
    bool oneTimeSubmit;
    bool simultaneousUse;
    bool decoded;
} TextureDecoder_CommandBufferState;

typedef struct TextureDecoder {
    SparseArray64 images;
    SparseArray64 boundBuffers;
    ArrayDeque bufferImageCopies;
    ThreadPool* threadPool;
    uint32_t deviceSupportedFormats;
    short imageCacheSize;

    VkContext* context;
    VkDevice device;
    VkQueue queue;
    VkCommandPool commandPool;
    int uploadMode;

    /* BATCHED / BATCHED_SAFE / SYNC */
    TextureDecoder_StagingSlot slots[TEXTURE_DECODER_STAGING_SLOTS];
    int currentSlot;
    ArrayList slotImages; /* images written by the slot being recorded (BATCHED_SAFE) */

    /* INLINE */
    SparseArray64 commandBuffers; /* VkCommandBuffer -> TextureDecoder_CommandBufferState* */
    ArrayList freeChunks;
    VkDeviceSize freeChunkBytes;

    bool cacheSizeLoaded;
    uint64_t cacheSizeBytes;

    /* Counters (diagnostics, logged periodically). */
    uint64_t statRegions;
    uint64_t statCacheHits;
    uint64_t statSubmits;
    uint64_t statSlotWaits;
    uint64_t statWAWBarriers;
    uint64_t statDecodeNanos;
    uint64_t statStagingBytes;
} TextureDecoder;

extern TextureDecoder* TextureDecoder_create(VkContext* context, VkPhysicalDevice physicalDevice, VkDevice device, VkPhysicalDeviceFeatures* supportedFeatures);
extern void TextureDecoder_destroy(TextureDecoder* textureDecoder);
/* For a client that disconnected without vkDestroyDevice: destroys only if the GPU queue goes idle. */
extern void TextureDecoder_destroyDetached(TextureDecoder* textureDecoder);

/* Called by vkQueueSubmit/vkQueueSubmit2 BEFORE the real submit. Returns VK_SUCCESS or the error
 * that made the uploads impossible (e.g. VK_ERROR_DEVICE_LOST in the legacy modes). */
extern VkResult TextureDecoder_onQueueSubmit(TextureDecoder* textureDecoder, VkQueue submitQueue, uint32_t commandBufferCount, const VkCommandBuffer* commandBuffers);

extern void TextureDecoder_copyBufferToImage(TextureDecoder* textureDecoder, VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, VkBufferImageCopy* regions);
extern void TextureDecoder_copyBufferToImage2(TextureDecoder* textureDecoder, VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage, VkImageLayout dstImageLayout, VkCopyBufferToImageInfo2* copyBufferToImageInfo);

/* Command buffer lifecycle (INLINE mode bookkeeping; harmless in the other modes). */
extern void TextureDecoder_onAllocateCommandBuffers(TextureDecoder* textureDecoder, VkCommandPool commandPool, uint32_t count, const VkCommandBuffer* commandBuffers);
extern void TextureDecoder_onFreeCommandBuffers(TextureDecoder* textureDecoder, uint32_t count, const VkCommandBuffer* commandBuffers);
extern void TextureDecoder_onBeginCommandBuffer(TextureDecoder* textureDecoder, VkCommandBuffer commandBuffer, VkCommandBufferUsageFlags flags);
extern void TextureDecoder_onResetCommandBuffer(TextureDecoder* textureDecoder, VkCommandBuffer commandBuffer);
extern void TextureDecoder_onResetCommandPool(TextureDecoder* textureDecoder, VkCommandPool commandPool);
extern void TextureDecoder_onDestroyCommandPool(TextureDecoder* textureDecoder, VkCommandPool commandPool);
extern void TextureDecoder_onExecuteCommands(TextureDecoder* textureDecoder, VkCommandBuffer primary, uint32_t count, const VkCommandBuffer* secondaries);

extern bool TextureDecoder_containsImage(TextureDecoder* textureDecoder, VkImage image);
extern VkResult TextureDecoder_createImage(TextureDecoder* textureDecoder, VkImageCreateInfo* imageInfo, VkImage* pImage);
extern void TextureDecoder_destroyImage(TextureDecoder* textureDecoder, VkImage image);
extern void TextureDecoder_addBoundBuffer(TextureDecoder* textureDecoder, ResourceMemory* memory, VkBuffer buffer, VkDeviceSize memoryOffset);
extern void TextureDecoder_removeBoundBuffer(TextureDecoder* textureDecoder, VkBuffer buffer);

/* True when the decoder handles this format (a BC format the device cannot sample natively). */
extern bool TextureDecoder_isFormatSupported(TextureDecoder* textureDecoder, VkFormat format);
/* True for every BC1-BC7 format (formats this decoder is able to decode). */
extern bool TextureDecoder_canDecodeFormat(VkFormat format);
extern bool isCompressedFormat(VkFormat format);

/*
 * Uncompressed format used to store a decoded BC format:
 *   BC1/BC2/BC3 UNORM -> B8G8R8A8_UNORM, SRGB -> B8G8R8A8_SRGB, BC4 -> R8_[US]NORM,
 *   BC5 -> R8G8_[US]NORM, BC6H -> R16G16B16A16_SFLOAT, BC7 UNORM -> R8G8B8A8_UNORM, SRGB -> R8G8B8A8_SRGB.
 */
extern VkFormat TextureDecoder_getDecompressedFormat(VkFormat format);
extern uint32_t TextureDecoder_getDecompressedTexelSize(VkFormat compressedFormat);

#endif
