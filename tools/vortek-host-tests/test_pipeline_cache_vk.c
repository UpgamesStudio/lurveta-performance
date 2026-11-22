/*
 * Host test for Vortek's persistent pipeline cache (pipeline_cache.c, the code shipped in the APK),
 * on Mesa lavapipe with the Khronos validation layer.
 *
 *  run 1: empty cache, 48 distinct compute pipelines, destroy -> file written;
 *  run 2: file loaded intact (and, on drivers that store pipelines, every pipeline is a cache hit);
 *  run 3: file corrupted (one byte flipped) -> rejected, deleted, empty cache, pipelines still work;
 *  run 4: file from "another driver" (pipelineCacheUUID changed) -> rejected.
 *
 * lavapipe is not Mali: this checks behaviour, not the compile time saved on the phone.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <time.h>

#include "vk_context.h"
#include "vulkan_helper.h"
#include "pipeline_cache.h"

VulkanWrapper vulkanWrapper;
static int validationErrors = 0;

uint32_t getMemoryTypeIndex(uint32_t typeBits, VkFlags properties) { (void)typeBits; (void)properties; return 0; }
uint32_t getMemoryPropertyFlags(uint32_t memoryTypeIndex) { (void)memoryTypeIndex; return 0; }

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    (void)type; (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        validationErrors++;
        fprintf(stderr, "VALIDATION: %.300s\n", data->pMessage);
    }
    return VK_FALSE;
}

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d %s -> %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)

static VkInstance instance;
static VkPhysicalDevice physicalDevice;
static VkDebugUtilsMessengerEXT messenger;

static void createInstance(void) {
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) exit(2);
    initVulkanWrapper(&vulkanWrapper, lib);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char* extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledLayerCount = 1;
    info.ppEnabledLayerNames = layers;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = extensions;
    CHECK(vulkanWrapper.vkCreateInstance(&info, NULL, &instance));
    loadVulkanInstanceFuncs(&vulkanWrapper, instance);
    PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)vulkanWrapper.vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    VkDebugUtilsMessengerCreateInfoEXT messengerInfo = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
    messengerInfo.pfnUserCallback = debugCallback;
    CHECK(create(instance, &messengerInfo, NULL, &messenger));
    uint32_t count = 1;
    vulkanWrapper.vkEnumeratePhysicalDevices(instance, &count, &physicalDevice);
}

static VkDevice createDevice(void) {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    VkDevice device;
    CHECK(vulkanWrapper.vkCreateDevice(physicalDevice, &deviceInfo, NULL, &device));
    loadVulkanDeviceFuncs(&vulkanWrapper, device);
    return device;
}

static uint32_t* loadSpirv(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); *size = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t* code = malloc(*size);
    if (fread(code, 1, *size, f) != *size) exit(2);
    fclose(f);
    return code;
}

typedef struct RunResult { int pipelines; int cacheHits; double millis; size_t loadedBytes; } RunResult;

#define PIPELINE_COUNT 48

/* Same steps as AsyncPipelineCreator: the app passes VK_NULL_HANDLE, Vortek substitutes its cache. */
static RunResult runOnce(const uint32_t* spirv, size_t spirvSize) {
    RunResult res = {0};
    VkDevice device = createDevice();
    PersistentPipelineCache* cache = PersistentPipelineCache_create(physicalDevice, device);
    if (!cache) { fprintf(stderr, "no cache\n"); exit(2); }
    res.loadedBytes = cache->loadedBytes;

    VkShaderModuleCreateInfo moduleInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = spirvSize;
    moduleInfo.pCode = spirv;
    VkShaderModule module;
    CHECK(vulkanWrapper.vkCreateShaderModule(device, &moduleInfo, NULL, &module));
    VkPipelineLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout;
    CHECK(vulkanWrapper.vkCreatePipelineLayout(device, &layoutInfo, NULL, &layout));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint32_t i = 0; i < PIPELINE_COUNT; i++) {
        uint32_t value = i;
        VkSpecializationMapEntry entry = {0, 0, sizeof(uint32_t)};
        VkSpecializationInfo spec = {1, &entry, sizeof(uint32_t), &value};
        VkPipelineCreationFeedback feedback = {0}, stageFeedback = {0};
        VkPipelineCreationFeedbackCreateInfo feedbackInfo = {VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};
        feedbackInfo.pPipelineCreationFeedback = &feedback;
        feedbackInfo.pipelineStageCreationFeedbackCount = 1;
        feedbackInfo.pPipelineStageCreationFeedbacks = &stageFeedback;

        VkComputePipelineCreateInfo info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.pNext = &feedbackInfo;
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";
        info.stage.pSpecializationInfo = &spec;
        info.layout = layout;

        VkPipelineCache appCache = VK_NULL_HANDLE;   /* what DXVK passes */
        VkPipelineCache used = appCache ? appCache : cache->cache;
        VkPipeline pipeline;
        CHECK(vulkanWrapper.vkCreateComputePipelines(device, used, 1, &info, NULL, &pipeline));
        PersistentPipelineCache_onPipelinesCreated(cache, 1);
        res.pipelines++;
        if ((feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) && (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT)) res.cacheHits++;
        vulkanWrapper.vkDestroyPipeline(device, pipeline, NULL);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    res.millis = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    vulkanWrapper.vkDestroyPipelineLayout(device, layout, NULL);
    vulkanWrapper.vkDestroyShaderModule(device, module, NULL);
    PersistentPipelineCache_destroy(cache);   /* what vt_handle_vkDestroyDevice does */
    vulkanWrapper.vkDestroyDevice(device, NULL);
    return res;
}

static long fileSize(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s shader.comp.spv\n", argv[0]); return 2; }
    size_t spirvSize;
    uint32_t* spirv = loadSpirv(argv[1], &spirvSize);
    createInstance();

    VkPhysicalDeviceProperties props;
    vulkanWrapper.vkGetPhysicalDeviceProperties(physicalDevice, &props);
    char path[512];
    snprintf(path, sizeof(path), PIPELINE_CACHE_DIR "/pipeline-%x-%x.bin", props.vendorID, props.deviceID);
    mkdir(PIPELINE_CACHE_DIR, 0771);
    unlink(path);
    int failures = 0;

    RunResult r1 = runOnce(spirv, spirvSize);
    long size1 = fileSize(path);
    printf("run 1 (empty):     loaded %6zu B, %d pipelines, %2d cache hits, %7.1f ms, file %ld B\n", r1.loadedBytes, r1.pipelines, r1.cacheHits, r1.millis, size1);
    if (r1.loadedBytes != 0 || size1 <= 0) failures++;

    RunResult r2 = runOnce(spirv, spirvSize);
    printf("run 2 (reloaded):  loaded %6zu B, %d pipelines, %2d cache hits, %7.1f ms\n", r2.loadedBytes, r2.pipelines, r2.cacheHits, r2.millis);
    /* The file header is 56 bytes; everything after it must come back to the driver intact. */
    if (r2.loadedBytes == 0 || (long)r2.loadedBytes != size1 - 56) failures++;
    if (r2.loadedBytes <= 32)
        printf("  note: this driver keeps no pipelines in VkPipelineCache (lavapipe stores only the 32-byte header),\n"
               "        so cache hits cannot be observed here; they must be measured on the Mali device.\n");
    else if (r2.cacheHits != PIPELINE_COUNT) failures++;

    /* corrupt one byte of the driver data */
    FILE* f = fopen(path, "r+b");
    fseek(f, size1 - 7, SEEK_SET); int c = fgetc(f); fseek(f, size1 - 7, SEEK_SET); fputc(c ^ 0x5A, f); fclose(f);
    RunResult r3 = runOnce(spirv, spirvSize);
    printf("run 3 (corrupted): loaded %6zu B, %d pipelines, %2d cache hits  -> %s\n", r3.loadedBytes, r3.pipelines, r3.cacheHits, r3.loadedBytes == 0 ? "rejected" : "ACCEPTED");
    if (r3.loadedBytes != 0 || r3.pipelines != PIPELINE_COUNT) failures++;

    /* file written by "another driver": change the stored pipelineCacheUUID */
    f = fopen(path, "r+b");
    fseek(f, 20, SEEK_SET); c = fgetc(f); fseek(f, 20, SEEK_SET); fputc(c ^ 0xFF, f); fclose(f);
    RunResult r4 = runOnce(spirv, spirvSize);
    printf("run 4 (other UUID): loaded %6zu B -> %s\n", r4.loadedBytes, r4.loadedBytes == 0 ? "rejected" : "ACCEPTED");
    if (r4.loadedBytes != 0) failures++;

    free(spirv);
    printf("validation errors: %d\n", validationErrors);
    printf("%s\n", failures == 0 && validationErrors == 0 ? "PIPELINE CACHE: OK" : "PIPELINE CACHE: FAILED");
    return failures == 0 && validationErrors == 0 ? 0 : 1;
}
