#ifndef VORTEK_CONTEXT_H
#define VORTEK_CONTEXT_H

#include "vortek.h"
#include "xwindow_swapchain.h"
#include "texture_decoder.h"
#include "async_pipeline_creator.h"
#include "timeline_semaphore.h"
#include "pipeline_cache.h"

typedef struct VkContext {
    int clientFd;
    int vkMaxVersion;
    short maxDeviceMemory;
    short imageCacheSize;
    ResourceMemoryType resourceMemoryType;
    ArrayList* exposedDeviceExtensions;
    ArrayList* disabledDeviceExtensions;
    uint64_t totalAllocationSize;

    bool hasExternalMemoryFd;
    bool hasExternalMemoryDMABuf;

#if ENABLE_VALIDATION_LAYER
    VkDebugReportCallbackEXT debugReportCallback;
#endif

    char* inputBuffer;
    int inputBufferSize;
    MemoryPool memoryPool;

    pthread_t requestHandlerThread;
    RingBuffer* clientRing;
    RingBuffer* serverRing;
    VkResult status;

    int graphicsQueueIndex;
    bool performanceHints;           // LURVETA: thread priority + ADPF
    int64_t targetWorkDurationNanos; // LURVETA: ADPF target (frame budget)

    TextureDecoder* textureDecoder;
    ShaderInspector* shaderInspector;
    ThreadPool* threadPool;
    ThreadPool* decodeThreadPool; // LURVETA: BC decoding only (never shares workers with pipeline compiles)
    int textureUploadMode;        // LURVETA: TEXTURE_UPLOAD_MODE_* (texture_decoder.h)
    volatile bool shuttingDown;   // LURVETA: set by destroyVkContext; a normal shutdown is not an error
    int diagLines;
    uint32_t presentCount;        // LURVETA: the first present is logged as a launch stage
    bool errorReported;           // LURVETA: fatal error already logged and signalled to the client
    bool persistentPipelineCache; // LURVETA: option "pipelineCache"
    PersistentPipelineCache* pipelineCache;

    ArrayList extraDataRequests;
    pthread_mutex_t extraDataRequestsMutex;

    JMethods jmethods;
    char* engineName;
    uint32_t engineVersion;
    VkDriverId driverID;
} VkContext;

typedef struct ExtraDataRequest {
    uint16_t requestId;
    void* data;
    int size;
} ExtraDataRequest;

extern VkContext* createVkContext(JNIEnv *env, jobject obj, int clientFd, jobject options);
extern void destroyVkContext(JNIEnv *env, VkContext* context);
extern bool handleExtraDataRequest(VkContext* context, uint16_t requestId, int requestLength);

/*
 * LURVETA: records a fatal error (typically VK_ERROR_DEVICE_LOST) and stops the server cleanly:
 * logs it, appends it to APP_FILES_DIR/logs/vortek-errors.log, sets context->status and marks both
 * rings with RING_STATUS_EXIT so the client gets VK_ERROR_DEVICE_LOST instead of waiting forever.
 */
extern void VkContext_reportError(VkContext* context, const char* where, VkResult result);

/*
 * LURVETA diagnostics: records a Vulkan call that FAILED (logcat + files/logs/vortek-diagnostics.log).
 * Costs nothing when calls succeed; at most VORTEK_DIAG_MAX_LINES lines per session.
 * VkContext_logInfo writes one informational line (device summary at startup), same limits.
 */
extern void VkContext_logFailure(VkContext* context, const char* call, VkResult result, const char* fmt, ...);
extern void VkContext_logInfo(VkContext* context, const char* fmt, ...);

#endif
