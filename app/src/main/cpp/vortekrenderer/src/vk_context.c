#include <time.h>
#include <sys/stat.h>
#include "vk_context.h"
#include "vulkan_helper.h"
#include "request_handler.h"
#include "sysvshared_memory.h"
#include "string_utils.h"
#include "jni_utils.h"
#include "perf_hint.h"

#ifndef LURVETA_BUILD_ID
#define LURVETA_BUILD_ID "unknown"
#endif

static void loadJMethods(JMethods* jmethods) {
    JNIEnv* env;
    (*jmethods->jvm)->AttachCurrentThread(jmethods->jvm, &env, NULL);
    jmethods->env = env;

    jclass cls = (*env)->GetObjectClass(env, jmethods->obj);
    jmethods->getWindowWidth = (*env)->GetMethodID(env, cls, "getWindowWidth", "(I)I");
    jmethods->getWindowHeight = (*env)->GetMethodID(env, cls, "getWindowHeight", "(I)I");
    jmethods->getWindowHardwareBuffer = (*env)->GetMethodID(env, cls, "getWindowHardwareBuffer", "(IZ)J");
    jmethods->updateWindowContent = (*env)->GetMethodID(env, cls, "updateWindowContent", "(I)V");
}

static ExtraDataRequest* waitForExtraDataRequest(VkContext* context, uint16_t requestId) {
    ExtraDataRequest* result = NULL;
    uint32_t busyWaitIter = 0;

    while (context->status >= 0) {
        result = NULL;
        pthread_mutex_lock(&context->extraDataRequestsMutex);
        for (int i = 0; i < context->extraDataRequests.size; i++) {
            ExtraDataRequest* extraDataRequest = context->extraDataRequests.elements[i];
            if (extraDataRequest->requestId == requestId) {
                result = extraDataRequest;
                ArrayList_removeAt(&context->extraDataRequests, i);
                break;
            }
        }
        pthread_mutex_unlock(&context->extraDataRequestsMutex);

        if (result) break;
        busyWait(&busyWaitIter);
    }

    return context->status >= 0 ? result : NULL;
}

static inline int64_t monotonicNanos() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

#define VORTEK_ERROR_LOG_DIR "/data/data/" APP_ID "/files/logs"
#define VORTEK_ERROR_LOG VORTEK_ERROR_LOG_DIR "/vortek-errors.log"
#define DECODE_THREAD_POOL_NUM_THREADS 4

static const char* resultToString(VkResult result) {
    switch (result) {
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        default: return "VK_ERROR";
    }
}

void VkContext_reportError(VkContext* context, const char* where, VkResult result) {
    if (result >= 0 || context->shuttingDown) return;
    context->status = result;
    if (context->errorReported) return;
    context->errorReported = true;

    println("vortek: FATAL %s (%d) in %s: stopping the Vulkan server and notifying the client", resultToString(result), result, where);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    mkdir(VORTEK_ERROR_LOG_DIR, 0771);
    FILE* file = fopen(VORTEK_ERROR_LOG, "a");
    if (file) {
        fprintf(file, "%lld %s (%d) in %s, driver %d, engine %s\n", (long long)ts.tv_sec, resultToString(result), result, where,
                (int)context->driverID, context->engineName ? context->engineName : "-");
        fclose(file);
    }

    // The client reads replies from clientRing: with EXIT set its wait returns VK_ERROR_DEVICE_LOST.
    if (context->clientRing) RingBuffer_setStatus(context->clientRing, RING_STATUS_EXIT);
    if (context->serverRing) RingBuffer_setStatus(context->serverRing, RING_STATUS_EXIT);
}

#define VORTEK_DIAG_LOG VORTEK_ERROR_LOG_DIR "/vortek-diagnostics.log"
#define JOURNAL_SIZE 4000
#define JOURNAL_NO_REPLY 0x7fffffff

typedef struct JournalEntry {
    int16_t request;
    int32_t result;
    uint32_t count;
    uint32_t firstMs;
} JournalEntry;
#define VORTEK_DIAG_MAX_LINES 200

static void diagWrite(VkContext* context, const char* kind, const char* call, VkResult result, const char* fmt, va_list args) {
    if (context->diagLines >= VORTEK_DIAG_MAX_LINES) return;
    context->diagLines++;

    char detail[400];
    vsnprintf(detail, sizeof(detail), fmt, args);
    char line[600];
    if (call) snprintf(line, sizeof(line), "%s %s -> %s (%d) %s", kind, call, resultToString(result), result, detail);
    else snprintf(line, sizeof(line), "%s %s", kind, detail);

    println("vortek-diag: %s", line);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    mkdir(VORTEK_ERROR_LOG_DIR, 0771);
    FILE* file = fopen(VORTEK_DIAG_LOG, "a");
    if (file) {
        fprintf(file, "%lld %s\n", (long long)ts.tv_sec, line);
        fclose(file);
    }
    if (context->diagLines == VORTEK_DIAG_MAX_LINES) println("vortek-diag: line limit reached, further diagnostics of this session are dropped");
}

void VkContext_logFailure(VkContext* context, const char* call, VkResult result, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    diagWrite(context, "FAIL", call, result, fmt, args);
    va_end(args);
}

void VkContext_logInfo(VkContext* context, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    diagWrite(context, "INFO", NULL, VK_SUCCESS, fmt, args);
    va_end(args);
}

// One file per Vulkan client process: vortek-activity-<pid>.txt (several clients can be connected).

static void journalRecord(VkContext* context, int request, int result) {
    if (!context->journal) return;
    pthread_mutex_lock(&context->journalMutex);
    int last = (context->journalHead + JOURNAL_SIZE - 1) % JOURNAL_SIZE;
    JournalEntry* entry = context->journalCount > 0 ? &context->journal[last] : NULL;
    if (entry && entry->request == request && entry->result == result) {
        entry->count++;
    }
    else {
        entry = &context->journal[context->journalHead];
        entry->request = (int16_t)request;
        entry->result = result;
        entry->count = 1;
        entry->firstMs = (uint32_t)((monotonicNanos() - context->connectNanos) / 1000000);
        context->journalHead = (context->journalHead + 1) % JOURNAL_SIZE;
        if (context->journalCount < JOURNAL_SIZE) context->journalCount++;
    }
    context->journalVersion++;
    pthread_mutex_unlock(&context->journalMutex);

    // The first real error of a call (capability queries legitimately answer "not supported").
    const char* name = requestCodeToString((short)request);
    if (result < 0 && result != JOURNAL_NO_REPLY && !context->firstErrorLogged && !strstr(name, "GetPhysicalDevice") && !strstr(name, "Enumerate")) {
        context->firstErrorLogged = true;
        VkContext_logFailure(context, name, (VkResult)result, "FIRST ERROR returned to this client (see vortek-journal-%d.txt)", context->clientPid);
    }
}

static void journalWrite(VkContext* context) {
    if (!context->journal) return;
    char tmpPath[sizeof(context->journalPath) + 8];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", context->journalPath);
    FILE* file = fopen(tmpPath, "w");
    if (!file) return;
    pthread_mutex_lock(&context->journalMutex);
    fprintf(file, "# Vortek call journal, client pid %d, build %s: time since connection, call, x repetitions, result returned\n", context->clientPid, LURVETA_BUILD_ID);
    int start = (context->journalHead + JOURNAL_SIZE - context->journalCount) % JOURNAL_SIZE;
    for (int i = 0; i < context->journalCount; i++) {
        JournalEntry* entry = &context->journal[(start + i) % JOURNAL_SIZE];
        char result[48];
        if (entry->result == JOURNAL_NO_REPLY) snprintf(result, sizeof(result), "-");
        else if (entry->result <= 0) snprintf(result, sizeof(result), "%s", resultToString((VkResult)entry->result));
        else snprintf(result, sizeof(result), "%d", entry->result);
        fprintf(file, "+%u.%03us %s x%u -> %s\n", entry->firstMs / 1000, entry->firstMs % 1000, requestCodeToString(entry->request), entry->count, result);
    }
    pthread_mutex_unlock(&context->journalMutex);
    fclose(file);
    rename(tmpPath, context->journalPath);
}

static void* activityThread(void* param) {
    VkContext* context = param;
    char tmpPath[sizeof(context->activityPath) + 8];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", context->activityPath);
    uint64_t writtenJournal = 0;
    while (!context->shuttingDown) {
        for (int i = 0; i < 20 && !context->shuttingDown; i++) usleep(100000);
        if (context->shuttingDown) break;

        int64_t now = monotonicNanos();
        int current = context->currentRequest;
        int last = context->lastRequest;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        FILE* file = fopen(tmpPath, "w");
        if (!file) continue;
        fprintf(file, "%lld pid=%d requests=%llu submits=%llu presents=%u swapchains=%u images=%u allocMB=%llu ",
                (long long)ts.tv_sec, context->clientPid, (unsigned long long)context->requestCount, (unsigned long long)context->submitCount,
                context->presentCount, context->swapchainCount, context->imageCount, (unsigned long long)(context->totalAllocationSize >> 20));
        if (current >= 0) {
            fprintf(file, "state=IN_CALL call=%s forMs=%lld\n", requestCodeToString((short)current), (long long)((now - context->requestStartNanos) / 1000000));
        }
        else {
            fprintf(file, "state=WAITING_FOR_GAME last=%s idleMs=%lld\n", last >= 0 ? requestCodeToString((short)last) : "-",
                    context->requestEndNanos ? (long long)((now - context->requestEndNanos) / 1000000) : -1LL);
        }
        fclose(file);
        rename(tmpPath, context->activityPath);
        if (context->journalVersion != writtenJournal) {
            writtenJournal = context->journalVersion;
            journalWrite(context);
        }
    }
    journalWrite(context);
    return NULL;
}

static void* requestHandlerThread(void* param) {
    VkContext* context = param;
    loadJMethods(&context->jmethods);
    ExtraDataRequest* extraDataRequest = NULL;

    // LURVETA: priority + ADPF. Work per frame = frame interval - time blocked waiting for requests.
    PerfHint perfHint;
    PerfHint_init(&perfHint, context->performanceHints, context->targetWorkDurationNanos);
    int64_t frameStart = monotonicNanos();
    uint64_t waitAtFrameStart = context->serverRing->waitNanos;

    while (context->status >= 0) {
        int requestCode = vt_recv(context->serverRing, &context->inputBuffer, &context->inputBufferSize, &context->memoryPool);
        if (requestCode < 0) break;

        if (requestCode > INT16_MAX) {
            uint16_t requestId = requestCode & 0xffff;
            requestCode = requestCode >> 16;
            extraDataRequest = waitForExtraDataRequest(context, requestId);
            if (!extraDataRequest) break;
            context->inputBufferSize = extraDataRequest->size;
            context->inputBuffer = extraDataRequest->data;
        }

#if DEBUG_MODE
        println("handleRequest name=%s size=%d", requestCodeToString(requestCode), context->inputBufferSize);
#endif

        HandleRequestFunc handleRequestFunc = getHandleRequestFunc(requestCode);
        context->requestStartNanos = monotonicNanos();
        context->currentRequest = requestCode;
        context->requestCount++;
        vt_replyCount = 0;
        if (handleRequestFunc) handleRequestFunc(context);
        journalRecord(context, requestCode, vt_replyCount > 0 ? vt_replyResult : JOURNAL_NO_REPLY);
        context->lastRequest = requestCode;
        context->currentRequest = -1;
        context->requestEndNanos = monotonicNanos();

        if (requestCode == REQUEST_CODE_VK_QUEUE_PRESENT_KHR && perfHint.session) {
            int64_t now = monotonicNanos();
            uint64_t waited = context->serverRing->waitNanos - waitAtFrameStart;
            int64_t interval = now - frameStart;
            PerfHint_report(&perfHint, interval > (int64_t)waited ? interval - (int64_t)waited : 0);
            frameStart = now;
            waitAtFrameStart = context->serverRing->waitNanos;
        }

        vt_free(&context->memoryPool);

        if (extraDataRequest) {
            MEMFREE(extraDataRequest->data);
            MEMFREE(extraDataRequest);
        }

        context->inputBuffer = NULL;
        context->inputBufferSize = 0;
    }

    // Leaving the loop for any reason other than a normal shutdown must never leave the client
    // blocked on a reply that will not come.
    if (context->shuttingDown) println("vortek: request loop stopped (session closed)");
    else if (context->status < 0) VkContext_reportError(context, "request loop", context->status);
    else if (!RingBuffer_hasStatus(context->serverRing, RING_STATUS_EXIT)) {
        println("vortek: request loop ended (connection closed)");
        RingBuffer_setStatus(context->clientRing, RING_STATUS_EXIT);
    }

    PerfHint_close(&perfHint);
    (*context->jmethods.jvm)->DetachCurrentThread(context->jmethods.jvm);
    vt_free(&context->memoryPool);
    return NULL;
}

static bool setupRingBuffers(VkContext* context) {
    int shmFds[2];
    shmFds[0] = ashmemCreateRegion("vt-server-ring", RingBuffer_getSHMemSize(SERVER_RING_BUFFER_SIZE));
    shmFds[1] = ashmemCreateRegion("vt-client-ring", RingBuffer_getSHMemSize(CLIENT_RING_BUFFER_SIZE));

    context->serverRing = RingBuffer_create(shmFds[0], SERVER_RING_BUFFER_SIZE);
    if (!context->serverRing) return false;

    context->clientRing = RingBuffer_create(shmFds[1], CLIENT_RING_BUFFER_SIZE);
    if (!context->clientRing) return false;

    int result = send_fds(context->clientFd, shmFds, 2, NULL, 0);
    close(shmFds[0]);
    close(shmFds[1]);

    if (result < 0) return false;

    pthread_create(&context->requestHandlerThread, NULL, requestHandlerThread, context);
    return true;
}

VkContext* createVkContext(JNIEnv* env, jobject obj, int clientFd, jobject options) {
    jobjectArray exposedDeviceExtensions = getJFieldByName(env, options, "exposedDeviceExtensions", JSIGNATURE_ARRAY_STRING).l;

    VkContext* context = calloc(1, sizeof(VkContext));
    context->clientFd = clientFd;
    context->vkMaxVersion = getJFieldByName(env, options, "vkMaxVersion", "I").i;
    context->maxDeviceMemory = getJFieldByName(env, options, "maxDeviceMemory", "S").s;
    context->imageCacheSize = getJFieldByName(env, options, "imageCacheSize", "S").s;
    context->resourceMemoryType = getJFieldByName(env, options, "resourceMemoryType", "B").b;
    context->performanceHints = getJFieldByName(env, options, "performanceHints", "Z").z;
    context->textureUploadMode = getJFieldByName(env, options, "textureUploadMode", "I").i;
    context->persistentPipelineCache = getJFieldByName(env, options, "persistentPipelineCache", "Z").z;
    TextureDecoder_setHighBCEnabled(getJFieldByName(env, options, "bcHighFormats", "Z").z);
    int targetFrameRate = getJFieldByName(env, options, "targetFrameRate", "I").i;
    context->targetWorkDurationNanos = 1000000000ll / (targetFrameRate > 0 ? targetFrameRate : 60);
    context->exposedDeviceExtensions = jstringArrayToCharArray(env, exposedDeviceExtensions);

    pthread_mutex_init(&context->extraDataRequestsMutex, NULL);

    context->memoryPool.data = calloc(MEMORY_POOL_MAX_SIZE, 1);
    context->threadPool = ThreadPool_init(THREAD_POOL_NUM_THREADS);
    context->decodeThreadPool = ThreadPool_init(DECODE_THREAD_POOL_NUM_THREADS);

    (*env)->GetJavaVM(env, &context->jmethods.jvm);
    context->jmethods.obj = (*env)->NewGlobalRef(env, obj);

    if (!setupRingBuffers(context)) {
        destroyVkContext(env, context);
        return NULL;
    }

    context->currentRequest = -1;
    context->lastRequest = -1;
    struct ucred cred = {0};
    socklen_t credLength = sizeof(cred);
    if (getsockopt(clientFd, SOL_SOCKET, SO_PEERCRED, &cred, &credLength) == 0) context->clientPid = cred.pid;
    snprintf(context->activityPath, sizeof(context->activityPath), VORTEK_ERROR_LOG_DIR "/vortek-activity-%d.txt", context->clientPid > 0 ? context->clientPid : -clientFd);
    snprintf(context->journalPath, sizeof(context->journalPath), VORTEK_ERROR_LOG_DIR "/vortek-journal-%d.txt", context->clientPid > 0 ? context->clientPid : -clientFd);
    context->journal = calloc(JOURNAL_SIZE, sizeof(JournalEntry));
    pthread_mutex_init(&context->journalMutex, NULL);
    context->connectNanos = monotonicNanos();
    mkdir(VORTEK_ERROR_LOG_DIR, 0771);
    pthread_create(&context->activityThread, NULL, activityThread, context);

    VkContext_logInfo(context, "STAGE client-connected pid %d build %s (upload mode %d, pipeline cache %d)", context->clientPid, LURVETA_BUILD_ID, context->textureUploadMode, (int)context->persistentPipelineCache);

    return context;
}

void destroyVkContext(JNIEnv* env, VkContext* context) {
    context->shuttingDown = true;
    context->status = VK_ERROR_DEVICE_LOST;

    if (context->activityThread) {
        pthread_join(context->activityThread, NULL);
        context->activityThread = 0;
        unlink(context->activityPath);
    }

    if (context->requestHandlerThread) {
        RingBuffer_setStatus(context->serverRing, RING_STATUS_EXIT);
        RingBuffer_setStatus(context->clientRing, RING_STATUS_EXIT);
        pthread_join(context->requestHandlerThread, NULL);

        ThreadPool_destroy(context->threadPool);
        context->threadPool = NULL;
        ThreadPool_destroy(context->decodeThreadPool);
        context->decodeThreadPool = NULL;

        context->requestHandlerThread = 0;
        RingBuffer_free(context->serverRing);
        RingBuffer_free(context->clientRing);
    }

    if (context->jmethods.obj) {
        (*env)->DeleteGlobalRef(env, context->jmethods.obj);
        context->jmethods.obj = NULL;
    }

    context->graphicsQueueIndex = 0;

    if (context->textureDecoder) {
        TextureDecoder_destroyDetached(context->textureDecoder);
        context->textureDecoder = NULL;
    }

    // The client went away without vkDestroyDevice (typical when Wine is killed): keep what was
    // compiled. The VkPipelineCache itself is left to the (leaked) device: a pipeline thread may
    // still be using it.
    if (context->pipelineCache) {
        PersistentPipelineCache_save(context->pipelineCache);
        context->pipelineCache = NULL;
    }

    ArrayList_free(context->exposedDeviceExtensions, true);
    context->exposedDeviceExtensions = NULL;

    ArrayList_free(context->disabledDeviceExtensions, true);
    context->disabledDeviceExtensions = NULL;

    FenceExport_free(&context->fenceExport);
    if (context->journal) {
        pthread_mutex_destroy(&context->journalMutex);
        MEMFREE(context->journal);
    }
    ArrayList_free(&context->extraDataRequests, true);
    pthread_mutex_destroy(&context->extraDataRequestsMutex);

    MEMFREE(context->memoryPool.data);
    MEMFREE(context->engineName);
    free(context);
}

bool handleExtraDataRequest(VkContext* context, uint16_t requestId, int requestLength) {
    void* data = NULL;
    if (requestLength > 0) {
        data = calloc(requestLength, 1);
        int bytesRead = sock_read(context->clientFd, data, requestLength);
        if (bytesRead != requestLength) return false;
    }

    ExtraDataRequest* extraDataRequest = calloc(1, sizeof(ExtraDataRequest));
    extraDataRequest->requestId = requestId;
    extraDataRequest->size = requestLength;
    extraDataRequest->data = data;

    pthread_mutex_lock(&context->extraDataRequestsMutex);
    ArrayList_add(&context->extraDataRequests, extraDataRequest);
    pthread_mutex_unlock(&context->extraDataRequestsMutex);
    return true;
}