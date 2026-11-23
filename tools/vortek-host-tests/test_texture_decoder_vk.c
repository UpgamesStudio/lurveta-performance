/*
 * Host integration test for Vortek's texture decoder (texture_decoder.c, the code shipped in the APK).
 *
 * Runs on Mesa lavapipe (a CPU Vulkan driver) with VK_LAYER_KHRONOS_validation and synchronization
 * validation enabled. lavapipe supports BC natively, so the test hands TextureDecoder_create() a
 * feature struct with textureCompressionBC = VK_FALSE: the decoder then behaves exactly as on Mali.
 *
 * IMPORTANT: lavapipe is not Mali. These tests check CORRECTNESS (pixels, Vulkan rules, lifecycle)
 * and STRUCTURE (extra submits / CPU waits), never Mali performance.
 *
 * Scenarios, run for every upload mode (inline, batched-safe, batched, sync):
 *  1. first upload of a fresh image (UNDEFINED -> TRANSFER_DST barrier recorded by the "game"),
 *     two overlapping copies separated by the game's own barrier, read back in the same command buffer;
 *  2. the same command buffer resubmitted after the game rewrote the source buffer;
 *  3. a copy recorded in a secondary command buffer executed through vkCmdExecuteCommands;
 *  4. BC7, BC6H, BC4 and BC5 images with two mip levels and odd sizes;
 *  5. reset / re-record / free / destroy-pool lifecycle (no leaks, staging recycled).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>

#include "vk_context.h"
#include "vulkan_helper.h"
#include "texture_decoder.h"
#include "bc_decoder.h"

VulkanWrapper vulkanWrapper;
static VkPhysicalDeviceMemoryProperties memoryProperties;
static PFN_vkCreateDebugUtilsMessengerEXT createDebugUtilsMessenger;
static PFN_vkDestroyDebugUtilsMessengerEXT destroyDebugUtilsMessenger;

static int validationErrors = 0;
static int validationWarnings = 0;
static char firstMessages[4][1024];
static int firstMessageCount = 0;

uint32_t getMemoryTypeIndex(uint32_t typeBits, VkFlags properties) {
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) return i;
    }
    return 0;
}

uint32_t getMemoryPropertyFlags(uint32_t memoryTypeIndex) {
    return memoryTypeIndex < memoryProperties.memoryTypeCount ? memoryProperties.memoryTypes[memoryTypeIndex].propertyFlags : 0;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    (void)type; (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) validationErrors++;
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) validationWarnings++;
    else return VK_FALSE;
    if (firstMessageCount < 4) snprintf(firstMessages[firstMessageCount++], sizeof(firstMessages[0]), "%s", data->pMessage);
    return VK_FALSE;
}

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d %s -> %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)

typedef struct Env {
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkQueue queue;
    uint32_t queueFamily;
    VkCommandPool pool;
    VkFence fence;
} Env;

static void createEnv(Env* env, bool validation) {
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { fprintf(stderr, "libvulkan.so.1 not found\n"); exit(2); }
    initVulkanWrapper(&vulkanWrapper, lib);

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    app.pApplicationName = "vortek-texture-decoder-test";

    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char* extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkValidationFeaturesEXT features = {VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    features.enabledValidationFeatureCount = 1;
    features.pEnabledValidationFeatures = enables;

    VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    if (validation) {
        info.pNext = &features;
        info.enabledLayerCount = 1;
        info.ppEnabledLayerNames = layers;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = extensions;
    }
    CHECK(vulkanWrapper.vkCreateInstance(&info, NULL, &env->instance));
    loadVulkanInstanceFuncs(&vulkanWrapper, env->instance);

    if (validation) {
        createDebugUtilsMessenger = (PFN_vkCreateDebugUtilsMessengerEXT)vulkanWrapper.vkGetInstanceProcAddr(env->instance, "vkCreateDebugUtilsMessengerEXT");
        destroyDebugUtilsMessenger = (PFN_vkDestroyDebugUtilsMessengerEXT)vulkanWrapper.vkGetInstanceProcAddr(env->instance, "vkDestroyDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        messengerInfo.pfnUserCallback = debugCallback;
        CHECK(createDebugUtilsMessenger(env->instance, &messengerInfo, NULL, &env->messenger));
    }

    uint32_t count = 1;
    VkResult result = vulkanWrapper.vkEnumeratePhysicalDevices(env->instance, &count, &env->physicalDevice);
    if (count == 0 || (result != VK_SUCCESS && result != VK_INCOMPLETE)) { fprintf(stderr, "no Vulkan device\n"); exit(2); }
    vulkanWrapper.vkGetPhysicalDeviceMemoryProperties(env->physicalDevice, &memoryProperties);

    VkPhysicalDeviceProperties props;
    vulkanWrapper.vkGetPhysicalDeviceProperties(env->physicalDevice, &props);
    printf("device: %s\n", props.deviceName);

    env->queueFamily = 0;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = env->queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    CHECK(vulkanWrapper.vkCreateDevice(env->physicalDevice, &deviceInfo, NULL, &env->device));
    loadVulkanDeviceFuncs(&vulkanWrapper, env->device);
    vulkanWrapper.vkGetDeviceQueue(env->device, env->queueFamily, 0, &env->queue);

    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = env->queueFamily;
    CHECK(vulkanWrapper.vkCreateCommandPool(env->device, &poolInfo, NULL, &env->pool));

    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vulkanWrapper.vkCreateFence(env->device, &fenceInfo, NULL, &env->fence));
}

static void destroyEnv(Env* env) {
    vulkanWrapper.vkDestroyFence(env->device, env->fence, NULL);
    vulkanWrapper.vkDestroyCommandPool(env->device, env->pool, NULL);
    vulkanWrapper.vkDestroyDevice(env->device, NULL);
    if (env->messenger) destroyDebugUtilsMessenger(env->instance, env->messenger, NULL);
    vulkanWrapper.vkDestroyInstance(env->instance, NULL);
}

/* ------------------------------------------------------------------------------------------- */

typedef struct SourceMemory {
    ResourceMemory resource;
    uint8_t* data;
    VkBuffer buffer; /* the game's staging buffer handle (only used as a key by the decoder) */
} SourceMemory;

static void createSource(Env* env, TextureDecoder* td, SourceMemory* src, size_t size) {
    memset(src, 0, sizeof(*src));
    int fd = memfd_create("vortek-test-src", 0);
    if (fd < 0 || ftruncate(fd, size) != 0) { perror("memfd"); exit(2); }
    src->resource.fd = fd;
    src->resource.allocationSize = size;
    src->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    CHECK(vulkanWrapper.vkCreateBuffer(env->device, &bufferInfo, NULL, &src->buffer));
    TextureDecoder_addBoundBuffer(td, &src->resource, src->buffer, 0);
}

static void destroySource(Env* env, TextureDecoder* td, SourceMemory* src) {
    TextureDecoder_removeBoundBuffer(td, src->buffer);
    vulkanWrapper.vkDestroyBuffer(env->device, src->buffer, NULL);
    munmap(src->data, src->resource.allocationSize);
    close(src->resource.fd);
}

typedef struct HostBuffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t* data;
    VkDeviceSize size;
} HostBuffer;

static void createReadback(Env* env, HostBuffer* hb, VkDeviceSize size) {
    VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    CHECK(vulkanWrapper.vkCreateBuffer(env->device, &bufferInfo, NULL, &hb->buffer));
    VkMemoryRequirements req;
    vulkanWrapper.vkGetBufferMemoryRequirements(env->device, hb->buffer, &req);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = getMemoryTypeIndex(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vulkanWrapper.vkAllocateMemory(env->device, &alloc, NULL, &hb->memory));
    CHECK(vulkanWrapper.vkBindBufferMemory(env->device, hb->buffer, hb->memory, 0));
    CHECK(vulkanWrapper.vkMapMemory(env->device, hb->memory, 0, VK_WHOLE_SIZE, 0, (void**)&hb->data));
    hb->size = size;
}

static void destroyReadback(Env* env, HostBuffer* hb) {
    vulkanWrapper.vkUnmapMemory(env->device, hb->memory);
    vulkanWrapper.vkDestroyBuffer(env->device, hb->buffer, NULL);
    vulkanWrapper.vkFreeMemory(env->device, hb->memory, NULL);
}

typedef struct TestImage {
    VkImage image;
    VkDeviceMemory memory;
    VkFormat format;
    uint32_t width, height, mips;
} TestImage;

static void createImage(Env* env, TextureDecoder* td, TestImage* img, VkFormat format, uint32_t width, uint32_t height, uint32_t mips) {
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = (VkExtent3D){width, height, 1};
    info.mipLevels = mips;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!TextureDecoder_isFormatSupported(td, format)) { fprintf(stderr, "format %d not routed to the decoder\n", format); exit(2); }
    CHECK(TextureDecoder_createImage(td, &info, &img->image));

    VkMemoryRequirements req;
    vulkanWrapper.vkGetImageMemoryRequirements(env->device, img->image, &req);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = getMemoryTypeIndex(req.memoryTypeBits, 0);
    CHECK(vulkanWrapper.vkAllocateMemory(env->device, &alloc, NULL, &img->memory));
    CHECK(vulkanWrapper.vkBindImageMemory(env->device, img->image, img->memory, 0));
    img->format = format;
    img->width = width;
    img->height = height;
    img->mips = mips;
}

static void destroyImage(Env* env, TextureDecoder* td, TestImage* img) {
    TextureDecoder_destroyImage(td, img->image);
    vulkanWrapper.vkFreeMemory(env->device, img->memory, NULL);
}

static void imageBarrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                         VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage, uint32_t mips) {
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    vulkanWrapper.vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void readbackBarrier(VkCommandBuffer cb) {
    VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vulkanWrapper.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
}

static VkCommandBuffer allocCommandBuffer(Env* env, TextureDecoder* td, VkCommandBufferLevel level) {
    VkCommandBufferAllocateInfo info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    info.commandPool = env->pool;
    info.level = level;
    info.commandBufferCount = 1;
    VkCommandBuffer cb;
    CHECK(vulkanWrapper.vkAllocateCommandBuffers(env->device, &info, &cb));
    TextureDecoder_onAllocateCommandBuffers(td, env->pool, 1, &cb);
    return cb;
}

static void beginCommandBuffer(TextureDecoder* td, VkCommandBuffer cb, VkCommandBufferUsageFlags flags, const VkCommandBufferInheritanceInfo* inheritance) {
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = flags;
    begin.pInheritanceInfo = inheritance;
    TextureDecoder_onBeginCommandBuffer(td, cb, flags);
    CHECK(vulkanWrapper.vkBeginCommandBuffer(cb, &begin));
}

/* What vt_handle_vkQueueSubmit does: decode first, then the real submit, then a bounded wait. */
static VkResult submitAndWait(Env* env, TextureDecoder* td, VkCommandBuffer cb) {
    VkResult result = TextureDecoder_onQueueSubmit(td, env->queue, 1, &cb);
    if (result != VK_SUCCESS) return result;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    CHECK(vulkanWrapper.vkQueueSubmit(env->queue, 1, &submit, env->fence));
    CHECK(vulkanWrapper.vkWaitForFences(env->device, 1, &env->fence, VK_TRUE, 10000000000ull));
    CHECK(vulkanWrapper.vkResetFences(env->device, 1, &env->fence));
    return VK_SUCCESS;
}

static void fillRandom(uint8_t* p, size_t n, unsigned* seed, int bcN) {
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(rand_r(seed) >> 7);
    if (bcN == 7) for (size_t i = 0; i < n; i += 16) if (p[i] == 0) p[i] = 1;          /* avoid reserved BC7 mode */
    if (bcN == 6) for (size_t i = 0; i < n; i += 16) if ((p[i] & 3) >= 2 && ((p[i] & 0x1f) == 0x13 || (p[i] & 0x1f) == 0x17 || (p[i] & 0x1f) == 0x1b || (p[i] & 0x1f) == 0x1f)) p[i] = 0x03;
}

static int bcOf(VkFormat format, int* blockSize, bool* noAlphaU) {
    switch (format) {
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: *blockSize = 8; *noAlphaU = false; return 1;
        case VK_FORMAT_BC4_UNORM_BLOCK: *blockSize = 8; *noAlphaU = true; return 4;
        case VK_FORMAT_BC5_UNORM_BLOCK: *blockSize = 16; *noAlphaU = true; return 5;
        case VK_FORMAT_BC6H_UFLOAT_BLOCK: *blockSize = 16; *noAlphaU = true; return 6;
        case VK_FORMAT_BC7_UNORM_BLOCK: *blockSize = 16; *noAlphaU = false; return 7;
        default: *blockSize = 16; *noAlphaU = false; return 3;
    }
}

static size_t compressedSize(uint32_t w, uint32_t h, int blockSize) {
    return (size_t)((w + 3) / 4) * ((h + 3) / 4) * blockSize;
}

/* CPU reference: decode `src` (w x h) and blit it into `dst` (dstWidth wide) at (x, y). */
static void referenceBlit(uint8_t* dst, uint32_t dstWidth, uint32_t x, uint32_t y, const uint8_t* src, uint32_t w, uint32_t h, VkFormat format) {
    int blockSize; bool noAlphaU;
    int bcN = bcOf(format, &blockSize, &noAlphaU);
    int texel = BCDecoder_getOutputTexelSize(bcN);
    uint8_t* tmp = malloc((size_t)w * h * texel);
    BCDecoder_decode(src, tmp, w, h, bcN, noAlphaU, NULL);
    for (uint32_t row = 0; row < h; row++) memcpy(dst + ((size_t)(y + row) * dstWidth + x) * texel, tmp + (size_t)row * w * texel, (size_t)w * texel);
    free(tmp);
}

static int compareBytes(const char* label, const uint8_t* a, const uint8_t* b, size_t n) {
    size_t bad = 0;
    for (size_t i = 0; i < n; i++) bad += a[i] != b[i];
    printf("    %-52s %s", label, bad ? "MISMATCH" : "ok");
    if (bad) printf(" (%zu of %zu bytes differ)", bad, n);
    printf("\n");
    return bad ? 1 : 0;
}

/* ------------------------------------------------------------------------------------------- */

static const char* modeNames[] = {"inline", "batched-safe", "batched", "sync"};

typedef struct ModeResult {
    int failures;
    int validationErrors;
    uint64_t extraSubmits;
    uint64_t cpuWaits;
    uint64_t wawBarriers;
} ModeResult;

static ModeResult runMode(Env* env, int mode) {
    ModeResult res = {0};
    int errorsBefore = validationErrors;
    firstMessageCount = 0;

    VkContext context = {0};
    context.graphicsQueueIndex = env->queueFamily;
    context.imageCacheSize = 0; /* no persistent cache in tests */
    context.decodeThreadPool = ThreadPool_init(4);
    context.textureUploadMode = mode;
    VkPhysicalDeviceFeatures noBC = {0};
    TextureDecoder* td = TextureDecoder_create(&context, env->physicalDevice, env->device, &noBC);
    if (!td) { fprintf(stderr, "decoder not created\n"); exit(2); }

    printf("\n== mode %s ==\n", modeNames[mode]);
    unsigned seed = 1234;

    /* ---- 1 + 2: first upload, overlapping copies, resubmit after source change ---------------- */
    {
        const uint32_t W = 64, H = 64;
        TestImage img; createImage(env, td, &img, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, W, H, 1);
        SourceMemory src; createSource(env, td, &src, 64 * 1024);
        HostBuffer rb; createReadback(env, &rb, W * H * 4);

        const size_t sizeA = compressedSize(W, H, 8), sizeB = compressedSize(32, 32, 8);
        fillRandom(src.data, sizeA, &seed, 1);
        fillRandom(src.data + 8192, sizeB, &seed, 1);

        VkCommandBuffer cb = allocCommandBuffer(env, td, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        beginCommandBuffer(td, cb, 0, NULL);
        imageBarrier(cb, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1);
        VkBufferImageCopy full = {0};
        full.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        full.imageExtent = (VkExtent3D){W, H, 1};
        TextureDecoder_copyBufferToImage(td, cb, src.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &full);
        /* the game's own write-after-write barrier between its two uploads */
        imageBarrier(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1);
        VkBufferImageCopy part = full;
        part.bufferOffset = 8192;
        part.imageOffset = (VkOffset3D){16, 16, 0};
        part.imageExtent = (VkExtent3D){32, 32, 1};
        TextureDecoder_copyBufferToImage(td, cb, src.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &part);
        imageBarrier(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1);
        VkBufferImageCopy back = full;
        vulkanWrapper.vkCmdCopyImageToBuffer(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &back);
        readbackBarrier(cb);
        CHECK(vulkanWrapper.vkEndCommandBuffer(cb));

        uint8_t* expected = malloc(W * H * 4);
        referenceBlit(expected, W, 0, 0, src.data, W, H, img.format);
        referenceBlit(expected, W, 16, 16, src.data + 8192, 32, 32, img.format);

        memset(rb.data, 0, rb.size);
        CHECK(submitAndWait(env, td, cb));
        res.failures += compareBytes("1. first upload + overlapping copies", rb.data, expected, W * H * 4);

        /* the game rewrites its staging data and resubmits the same (reusable) command buffer */
        fillRandom(src.data, sizeA, &seed, 1);
        referenceBlit(expected, W, 0, 0, src.data, W, H, img.format);
        referenceBlit(expected, W, 16, 16, src.data + 8192, 32, 32, img.format);
        memset(rb.data, 0, rb.size);
        CHECK(submitAndWait(env, td, cb));
        res.failures += compareBytes("2. resubmitted command buffer, new source data", rb.data, expected, W * H * 4);

        TextureDecoder_onFreeCommandBuffers(td, 1, &cb);
        vulkanWrapper.vkFreeCommandBuffers(env->device, env->pool, 1, &cb);
        free(expected);
        destroyReadback(env, &rb);
        destroySource(env, td, &src);
        destroyImage(env, td, &img);
    }

    /* ---- 3: secondary command buffer --------------------------------------------------------- */
    {
        const uint32_t W = 32, H = 32;
        TestImage img; createImage(env, td, &img, VK_FORMAT_BC3_UNORM_BLOCK, W, H, 1);
        SourceMemory src; createSource(env, td, &src, 16 * 1024);
        HostBuffer rb; createReadback(env, &rb, W * H * 4);
        fillRandom(src.data, compressedSize(W, H, 16), &seed, 3);

        VkCommandBuffer secondary = allocCommandBuffer(env, td, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
        VkCommandBufferInheritanceInfo inheritance = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
        beginCommandBuffer(td, secondary, 0, &inheritance);
        VkBufferImageCopy full = {0};
        full.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        full.imageExtent = (VkExtent3D){W, H, 1};
        TextureDecoder_copyBufferToImage(td, secondary, src.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &full);
        CHECK(vulkanWrapper.vkEndCommandBuffer(secondary));

        VkCommandBuffer primary = allocCommandBuffer(env, td, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        beginCommandBuffer(td, primary, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL);
        imageBarrier(primary, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1);
        TextureDecoder_onExecuteCommands(td, primary, 1, &secondary);
        vulkanWrapper.vkCmdExecuteCommands(primary, 1, &secondary);
        imageBarrier(primary, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1);
        vulkanWrapper.vkCmdCopyImageToBuffer(primary, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &full);
        readbackBarrier(primary);
        CHECK(vulkanWrapper.vkEndCommandBuffer(primary));

        uint8_t* expected = malloc(W * H * 4);
        referenceBlit(expected, W, 0, 0, src.data, W, H, img.format);
        memset(rb.data, 0, rb.size);
        CHECK(submitAndWait(env, td, primary));
        res.failures += compareBytes("3. copy inside a secondary command buffer", rb.data, expected, W * H * 4);

        VkCommandBuffer cbs[2] = {primary, secondary};
        TextureDecoder_onFreeCommandBuffers(td, 2, cbs);
        vulkanWrapper.vkFreeCommandBuffers(env->device, env->pool, 2, cbs);
        free(expected);
        destroyReadback(env, &rb);
        destroySource(env, td, &src);
        destroyImage(env, td, &img);
    }

    /* ---- 4: BC7 / BC6H / BC4 / BC5, two mips, odd sizes -------------------------------------- */
    {
        const VkFormat formats[] = {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK};
        const char* names[] = {"4. BC7 37x53, 2 mips", "4. BC6H 37x53, 2 mips", "4. BC4 300x260, 2 mips (multi-threaded)", "4. BC5 37x53, 2 mips"};
        const uint32_t sizes[][2] = {{37, 53}, {37, 53}, {300, 260}, {37, 53}};
        for (int f = 0; f < 4; f++) {
            const uint32_t W = sizes[f][0], H = sizes[f][1];
            int blockSize; bool noAlphaU;
            int bcN = bcOf(formats[f], &blockSize, &noAlphaU);
            int texel = BCDecoder_getOutputTexelSize(bcN);
            const uint32_t W1 = W / 2, H1 = H / 2;
            const size_t size0 = compressedSize(W, H, blockSize), size1 = compressedSize(W1, H1, blockSize);
            const size_t offset1 = (size0 + 255) & ~(size_t)255;

            TestImage img; createImage(env, td, &img, formats[f], W, H, 2);
            SourceMemory src; createSource(env, td, &src, offset1 + size1 + 4096);
            const size_t out0 = (size_t)W * H * texel, out1 = (size_t)W1 * H1 * texel;
            HostBuffer rb; createReadback(env, &rb, out0 + out1);
            fillRandom(src.data, size0, &seed, bcN);
            fillRandom(src.data + offset1, size1, &seed, bcN);

            VkCommandBuffer cb = allocCommandBuffer(env, td, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
            beginCommandBuffer(td, cb, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL);
            imageBarrier(cb, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 2);
            VkBufferImageCopy regions[2] = {0};
            regions[0].imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            regions[0].imageExtent = (VkExtent3D){W, H, 1};
            regions[1].bufferOffset = offset1;
            regions[1].imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1};
            regions[1].imageExtent = (VkExtent3D){W1, H1, 1};
            TextureDecoder_copyBufferToImage(td, cb, src.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, regions);
            imageBarrier(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 2);
            VkBufferImageCopy backs[2] = {regions[0], regions[1]};
            backs[0].bufferOffset = 0;
            backs[1].bufferOffset = out0;
            vulkanWrapper.vkCmdCopyImageToBuffer(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 2, backs);
            readbackBarrier(cb);
            CHECK(vulkanWrapper.vkEndCommandBuffer(cb));

            uint8_t* expected = malloc(out0 + out1);
            referenceBlit(expected, W, 0, 0, src.data, W, H, img.format);
            referenceBlit(expected + out0, W1, 0, 0, src.data + offset1, W1, H1, img.format);
            memset(rb.data, 0, rb.size);
            CHECK(submitAndWait(env, td, cb));
            res.failures += compareBytes(names[f], rb.data, expected, out0 + out1);

            TextureDecoder_onFreeCommandBuffers(td, 1, &cb);
            vulkanWrapper.vkFreeCommandBuffers(env->device, env->pool, 1, &cb);
            free(expected);
            destroyReadback(env, &rb);
            destroySource(env, td, &src);
            destroyImage(env, td, &img);
        }
    }

    /* ---- 5: lifecycle: 200 frames of record/submit/reset with a pool reset in the middle ------ */
    {
        const uint32_t W = 128, H = 128;
        TestImage img; createImage(env, td, &img, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, W, H, 1);
        SourceMemory src; createSource(env, td, &src, 64 * 1024);
        fillRandom(src.data, compressedSize(W, H, 8), &seed, 1);
        VkCommandBuffer cb = allocCommandBuffer(env, td, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        uint64_t stagingPeak = 0;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        for (int frame = 0; frame < 200; frame++) {
            if (frame == 100) {
                TextureDecoder_onResetCommandPool(td, env->pool);
                CHECK(vulkanWrapper.vkResetCommandPool(env->device, env->pool, 0));
            }
            beginCommandBuffer(td, cb, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL);
            imageBarrier(cb, img.image, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1);
            VkBufferImageCopy full = {0};
            full.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            full.imageExtent = (VkExtent3D){W, H, 1};
            TextureDecoder_copyBufferToImage(td, cb, src.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &full);
            imageBarrier(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 1);
            layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            CHECK(vulkanWrapper.vkEndCommandBuffer(cb));
            CHECK(submitAndWait(env, td, cb));
            if (td->statStagingBytes > stagingPeak) stagingPeak = td->statStagingBytes;
        }
        printf("    %-52s %s (staging peak %llu KB)\n", "5. 200 frames record/submit/reset + pool reset",
               stagingPeak <= (uint64_t)TEXTURE_DECODER_MIN_STAGING_SIZE * TEXTURE_DECODER_STAGING_SLOTS + TEXTURE_DECODER_CHUNK_SIZE * 2 ? "ok" : "STAGING GROWS",
               (unsigned long long)(stagingPeak >> 10));
        if (stagingPeak > (uint64_t)TEXTURE_DECODER_MIN_STAGING_SIZE * TEXTURE_DECODER_STAGING_SLOTS + TEXTURE_DECODER_CHUNK_SIZE * 2) res.failures++;
        TextureDecoder_onFreeCommandBuffers(td, 1, &cb);
        vulkanWrapper.vkFreeCommandBuffers(env->device, env->pool, 1, &cb);
        destroySource(env, td, &src);
        destroyImage(env, td, &img);
    }

    res.extraSubmits = td->statSubmits;
    res.cpuWaits = td->statSlotWaits;
    res.wawBarriers = td->statWAWBarriers;
    CHECK(vulkanWrapper.vkDeviceWaitIdle(env->device));
    TextureDecoder_destroy(td);
    ThreadPool_destroy(context.decodeThreadPool);

    res.validationErrors = validationErrors - errorsBefore;
    printf("    validation/sync-validation errors: %d\n", res.validationErrors);
    for (int i = 0; i < firstMessageCount; i++) printf("      - %.400s\n", firstMessages[i]);
    return res;
}

int main(int argc, char** argv) {
    bool validation = !(argc > 1 && strcmp(argv[1], "--no-validation") == 0);
    Env env = {0};
    createEnv(&env, validation);

    /* The regression-isolation switch: BC6H/BC7 off = 1.0.0 behaviour, BC1-5 unaffected. */
    TextureDecoder_setHighBCEnabled(false);
    int switchFailures = TextureDecoder_canDecodeFormat(VK_FORMAT_BC7_UNORM_BLOCK) || TextureDecoder_canDecodeFormat(VK_FORMAT_BC6H_UFLOAT_BLOCK) ||
                         !TextureDecoder_canDecodeFormat(VK_FORMAT_BC1_RGBA_UNORM_BLOCK) || !TextureDecoder_canDecodeFormat(VK_FORMAT_BC5_UNORM_BLOCK);
    TextureDecoder_setHighBCEnabled(true);
    switchFailures += !TextureDecoder_canDecodeFormat(VK_FORMAT_BC7_SRGB_BLOCK);
    printf("BC6H/BC7 switch: %s\n", switchFailures ? "FAILED" : "ok");

    ModeResult results[4];
    for (int mode = 0; mode < 4; mode++) results[mode] = runMode(&env, mode);

    printf("\n%-14s %-9s %-18s %-14s %-12s %s\n", "mode", "failures", "validation errors", "extra submits", "CPU waits", "WAW barriers");
    for (int mode = 0; mode < 4; mode++) {
        printf("%-14s %-9d %-18d %-14llu %-12llu %llu\n", modeNames[mode], results[mode].failures, results[mode].validationErrors,
               (unsigned long long)results[mode].extraSubmits, (unsigned long long)results[mode].cpuWaits, (unsigned long long)results[mode].wawBarriers);
    }

    /* GTA V keeps several VkDevices alive: the decoder of a FIRST device stays alive while a second
     * device creates, uploads and samples its own BC textures through its own decoder. */
    printf("\n== two devices alive (one decoder per device) ==");
    VkContext firstContext = {0};
    firstContext.graphicsQueueIndex = env.queueFamily;
    firstContext.decodeThreadPool = ThreadPool_init(2);
    firstContext.textureUploadMode = TEXTURE_UPLOAD_MODE_INLINE;
    VkPhysicalDeviceFeatures noBC = {0};
    TextureDecoder* firstDecoder = TextureDecoder_create(&firstContext, env.physicalDevice, env.device, &noBC);
    Env second = {0};
    createEnv(&second, validation);
    ModeResult twoDevices = runMode(&second, TEXTURE_UPLOAD_MODE_INLINE);
    TextureDecoder_destroy(firstDecoder);
    destroyEnv(&second);
    printf("two devices: failures %d, validation errors %d\n", twoDevices.failures, twoDevices.validationErrors);

    destroyEnv(&env);
    /* The default (inline) mode must be fully correct and clean; legacy modes are reported, not gated. */
    return !switchFailures && results[TEXTURE_UPLOAD_MODE_INLINE].failures == 0 && results[TEXTURE_UPLOAD_MODE_INLINE].validationErrors == 0
           && firstDecoder && twoDevices.failures == 0 && twoDevices.validationErrors == 0 ? 0 : 1;
}
