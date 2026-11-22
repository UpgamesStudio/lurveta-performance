// Host integration test of vortekrenderer/src/texture_decoder.c on lavapipe (software Vulkan).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/mman.h>
#include "vulkan_helper.h"
#include "bc_decoder.h"

VulkanWrapper vulkanWrapper = {0};
static VkPhysicalDeviceMemoryProperties memProps;
uint32_t getMemoryPropertyFlags(uint32_t i) { return i < memProps.memoryTypeCount ? memProps.memoryTypes[i].propertyFlags : 0; }
uint32_t getMemoryTypeIndex(uint32_t typeBits, VkFlags props) {
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) { if ((typeBits & 1) && (memProps.memoryTypes[i].propertyFlags & props)) return i; typeBits >>= 1; }
    return 0;
}
static PFN_vkGetInstanceProcAddr gipa; static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst; static VkPhysicalDevice pdev; static VkDevice dev; static VkQueue queue; static uint32_t family;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
#define LI(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
#define LD(n) vulkanWrapper.n = (PFN_##n)gdpa(dev, #n)

static VkCommandPool tpool;
static VkCommandBuffer beginCmd(void) {
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, tpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb; vulkanWrapper.vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; vulkanWrapper.vkBeginCommandBuffer(cb, &bi); return cb;
}
static void endCmd(VkCommandBuffer cb) {
    vulkanWrapper.vkEndCommandBuffer(cb); VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vulkanWrapper.vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); vulkanWrapper.vkQueueWaitIdle(queue); vulkanWrapper.vkFreeCommandBuffers(dev, tpool, 1, &cb);
}
static void transition(VkImage img, uint32_t layers, VkImageLayout from, VkImageLayout to) {
    VkCommandBuffer cb = beginCmd();
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from; b.newLayout = to; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = img;
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vulkanWrapper.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b); endCmd(cb);
}
// Reads back layer `layer` of a decoded image into host memory.
static uint8_t* readback(VkImage img, uint32_t w, uint32_t h, uint32_t texel, uint32_t layer, uint32_t layers) {
    VkDeviceSize size = (VkDeviceSize)w * h * texel;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = size; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf; vulkanWrapper.vkCreateBuffer(dev, &bci, NULL, &buf); VkMemoryRequirements mr; vulkanWrapper.vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = mr.size; mai.memoryTypeIndex = getMemoryTypeIndex(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VkDeviceMemory mem; vulkanWrapper.vkAllocateMemory(dev, &mai, NULL, &mem); vulkanWrapper.vkBindBufferMemory(dev, buf, mem, 0);
    transition(img, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkCommandBuffer cb = beginCmd(); VkBufferImageCopy r = {0}; r.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1}; r.imageExtent = (VkExtent3D){w, h, 1};
    vulkanWrapper.vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r); endCmd(cb);
    transition(img, layers, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    void* p; vulkanWrapper.vkMapMemory(dev, mem, 0, size, 0, &p); uint8_t* out = malloc(size); memcpy(out, p, size);
    vulkanWrapper.vkUnmapMemory(dev, mem); vulkanWrapper.vkDestroyBuffer(dev, buf, NULL); vulkanWrapper.vkFreeMemory(dev, mem, NULL); return out;
}
typedef struct { VkFormat fmt; uint32_t w, h, layers; int bcN, block; bool noAlphaU; uint32_t texel; VkImage img; ResourceMemory res; uint8_t* data; size_t size; uint64_t fakeBuf; } Case;
static uint64_t nextFake = 0x1000;
static void setupCase(TextureDecoder* td, Case* c, unsigned seed) {
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ici.imageType = VK_IMAGE_TYPE_2D; ici.format = c->fmt; ici.extent = (VkExtent3D){c->w, c->h, 1};
    ici.mipLevels = 1; ici.arrayLayers = c->layers; ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkResult r = TextureDecoder_createImage(td, &ici, &c->img); CHECK(r == VK_SUCCESS, "createImage fmt %d -> %d", c->fmt, r);
    CHECK(ici.format == TextureDecoder_getDecompressedFormat(c->fmt), "mapped format %d", ici.format);
    VkMemoryRequirements mr; vulkanWrapper.vkGetImageMemoryRequirements(dev, c->img, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = mr.size; mai.memoryTypeIndex = getMemoryTypeIndex(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory m; vulkanWrapper.vkAllocateMemory(dev, &mai, NULL, &m); vulkanWrapper.vkBindImageMemory(dev, c->img, m, 0);
    transition(c->img, c->layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    c->size = (size_t)((c->w + 3) / 4) * ((c->h + 3) / 4) * c->block * c->layers;
    int fd = memfd_create("bc", 0); ftruncate(fd, c->size + 4096);
    c->data = mmap(NULL, c->size + 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    srand(seed); for (size_t i = 0; i < c->size; i++) c->data[4096 + i] = rand();     // data starts at bufferOffset 4096
    c->res.fd = fd; c->res.allocationSize = c->size + 4096; c->fakeBuf = nextFake++;
    TextureDecoder_addBoundBuffer(td, &c->res, (VkBuffer)c->fakeBuf, 0);
    VkBufferImageCopy reg = {0}; reg.bufferOffset = 4096; reg.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, c->layers}; reg.imageExtent = (VkExtent3D){c->w, c->h, 1};
    TextureDecoder_copyBufferToImage(td, NULL, (VkBuffer)c->fakeBuf, c->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
}
static void verifyCase(Case* c, const char* name) {
    size_t sliceC = (size_t)((c->w + 3) / 4) * ((c->h + 3) / 4) * c->block, sliceD = (size_t)c->w * c->h * c->texel;
    for (uint32_t l = 0; l < c->layers; l++) {
        uint8_t* expect = malloc(sliceD); BCDecoder_decode(c->data + 4096 + l * sliceC, expect, c->w, c->h, c->bcN, c->noAlphaU, NULL);
        uint8_t* got = readback(c->img, c->w, c->h, c->texel, l, c->layers);
        size_t bad = 0; for (size_t i = 0; i < sliceD; i++) bad += expect[i] != got[i];
        CHECK(bad == 0, "%s layer %u: %zu/%zu bytes differ", name, l, bad, sliceD);
        free(expect); free(got);
    }
}
int main(void) {
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW); gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app; CHECK(ci(&ici, NULL, &inst) == VK_SUCCESS, "instance");
    LI(vkEnumeratePhysicalDevices); LI(vkGetPhysicalDeviceQueueFamilyProperties); LI(vkCreateDevice); LI(vkGetPhysicalDeviceMemoryProperties); LI(vkGetPhysicalDeviceProperties);
    uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pdev); vkGetPhysicalDeviceMemoryProperties(pdev, &memProps);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pdev, &pp); printf("device: %s\n", pp.deviceName);
    uint32_t qn = 8; VkQueueFamilyProperties qf[8]; vkGetPhysicalDeviceQueueFamilyProperties(pdev, &qn, qf);
    for (family = 0; family < qn && !(qf[family].queueFlags & VK_QUEUE_GRAPHICS_BIT); family++);
    float prio = 1; VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci; CHECK(vkCreateDevice(pdev, &dci, NULL, &dev) == VK_SUCCESS, "device");
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    LD(vkCreateBuffer); LD(vkGetBufferMemoryRequirements); LD(vkAllocateMemory); LD(vkBindBufferMemory); LD(vkMapMemory); LD(vkUnmapMemory); LD(vkDestroyBuffer); LD(vkFreeMemory);
    LD(vkCreateCommandPool); LD(vkAllocateCommandBuffers); LD(vkCreateFence); LD(vkGetDeviceQueue); LD(vkGetFenceStatus); LD(vkWaitForFences); LD(vkResetFences);
    LD(vkCmdPipelineBarrier); LD(vkEndCommandBuffer); LD(vkBeginCommandBuffer); LD(vkResetCommandBuffer); LD(vkFlushMappedMemoryRanges); LD(vkQueueSubmit); LD(vkCmdCopyBufferToImage);
    LD(vkFreeCommandBuffers); LD(vkDestroyFence); LD(vkDestroyCommandPool); LD(vkCreateImage); LD(vkDestroyImage); LD(vkQueueWaitIdle); LD(vkCmdCopyImageToBuffer);
    LD(vkGetImageMemoryRequirements); LD(vkBindImageMemory);
    vulkanWrapper.vkGetDeviceQueue(dev, family, 0, &queue);
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.queueFamilyIndex = family; vulkanWrapper.vkCreateCommandPool(dev, &cpi, NULL, &tpool);

    VkContext ctx = {0}; ctx.imageCacheSize = 64; ctx.graphicsQueueIndex = family; ctx.threadPool = ThreadPool_init(4);
    VkPhysicalDeviceFeatures feats = {0}; feats.textureCompressionBC = VK_FALSE;
    TextureDecoder* td = TextureDecoder_create(&ctx, pdev, dev, &feats); CHECK(td && td->commandPool, "decoder create");

    Case cases[] = {
        {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 64, 64, 1, 1, 8, false, 4},
        {VK_FORMAT_BC3_UNORM_BLOCK, 30, 18, 1, 3, 16, false, 4},      // non multiple of 4
        {VK_FORMAT_BC4_UNORM_BLOCK, 128, 64, 1, 4, 8, true, 1},
        {VK_FORMAT_BC5_SNORM_BLOCK, 64, 64, 6, 5, 16, false, 2},      // 6 layers (cube-like)
        {VK_FORMAT_BC1_RGB_UNORM_BLOCK, 1024, 1024, 1, 1, 8, true, 4}, // cacheable (>=1024)
    };
    int nc = sizeof cases / sizeof *cases;
    for (int i = 0; i < nc; i++) setupCase(td, &cases[i], 100 + i);
    TextureDecoder_decodeAll(td, queue); vulkanWrapper.vkQueueWaitIdle(queue);
    const char* names[] = {"BC1_SRGB 64x64", "BC3 30x18", "BC4 128x64", "BC5_SNORM 64x64x6", "BC1 1024 (cache miss)"};
    for (int i = 0; i < nc; i++) verifyCase(&cases[i], names[i]);
    printf("batch 1: regions=%llu submits=%llu slotWaits=%llu cacheHits=%llu\n", (unsigned long long)td->statRegions, (unsigned long long)td->statSubmits, (unsigned long long)td->statSlotWaits, (unsigned long long)td->statCacheHits);
    CHECK(td->statSubmits == 1, "one submit for 5 regions (got %llu)", (unsigned long long)td->statSubmits);

    // Same 1024 texture again -> must come from the disk cache and still be correct.
    Case again = cases[4]; again.img = VK_NULL_HANDLE; setupCase(td, &again, 104);
    TextureDecoder_decodeAll(td, queue); vulkanWrapper.vkQueueWaitIdle(queue); verifyCase(&again, "BC1 1024 (cache hit)");
    CHECK(td->statCacheHits >= 1, "cache hit expected");

    // > 32 MB in one batch -> overflows into the second staging slot; different queue handle -> blocking path.
    Case big[3]; for (int i = 0; i < 3; i++) { big[i] = (Case){VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 2048, 2048, 1, 1, 8, false, 4}; setupCase(td, &big[i], 200 + i); }
    uint64_t s0 = td->statSubmits; TextureDecoder_decodeAll(td, (VkQueue)0x1); 
    for (int i = 0; i < 3; i++) verifyCase(&big[i], "BC1 2048 big batch");
    printf("big batch: submits=%llu slotWaits=%llu\n", (unsigned long long)(td->statSubmits - s0), (unsigned long long)td->statSlotWaits);
    CHECK(td->statSubmits - s0 >= 2, "big batch should use more than one staging slot");

    // Destroying an image with a pending copy must not touch freed memory (ASan).
    Case doomed = {VK_FORMAT_BC4_SNORM_BLOCK, 16, 16, 1, 4, 8, false, 1}; setupCase(td, &doomed, 300);
    TextureDecoder_destroyImage(td, doomed.img); TextureDecoder_removeBoundBuffer(td, (VkBuffer)doomed.fakeBuf);
    TextureDecoder_decodeAll(td, queue); vulkanWrapper.vkQueueWaitIdle(queue);

    TextureDecoder_destroy(td);
    printf(fails ? "%d FAILURES\n" : "ALL TEXTURE DECODER INTEGRATION TESTS PASSED\n", fails);
    return fails != 0;
}
