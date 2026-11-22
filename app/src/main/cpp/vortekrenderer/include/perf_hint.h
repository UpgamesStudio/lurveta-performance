#ifndef VORTEK_PERF_HINT_H
#define VORTEK_PERF_HINT_H

/*
 * LURVETA Performance — performance hints for the Vortek request thread (the thread that feeds
 * every Vulkan command of the game to the GPU driver).
 *
 *  - Thread priority: tries nice -4 (Android THREAD_PRIORITY_DISPLAY). If the platform does not
 *    allow it, the default priority is kept (no error).
 *  - ADPF (Android Dynamic Performance Framework, API 33+): reports the real CPU work time of each
 *    frame (frame interval minus the time spent blocked waiting for the game) against a target
 *    duration, so the CPU governor can raise clocks only when the frame needs it and lower them
 *    otherwise (less heat). Loaded with dlsym: on older Android versions this is a no-op.
 */

#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;

typedef struct PerfHint {
    void* lib;
    APerformanceHintSession* session;
    int (*reportActualWorkDuration)(APerformanceHintSession*, int64_t);
    void (*closeSession)(APerformanceHintSession*);
    bool priorityRaised;
} PerfHint;

static inline void PerfHint_init(PerfHint* perfHint, bool enabled, int64_t targetWorkDurationNanos) {
    memset(perfHint, 0, sizeof(PerfHint));
    if (!enabled) return;

    perfHint->priorityRaised = setpriority(PRIO_PROCESS, gettid(), -4) == 0;

    perfHint->lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!perfHint->lib) return;

    APerformanceHintManager* (*getManager)(void) = dlsym(perfHint->lib, "APerformanceHint_getManager");
    APerformanceHintSession* (*createSession)(APerformanceHintManager*, const int32_t*, size_t, int64_t) = dlsym(perfHint->lib, "APerformanceHint_createSession");
    perfHint->reportActualWorkDuration = dlsym(perfHint->lib, "APerformanceHint_reportActualWorkDuration");
    perfHint->closeSession = dlsym(perfHint->lib, "APerformanceHint_closeSession");

    if (getManager && createSession && perfHint->reportActualWorkDuration && perfHint->closeSession) {
        APerformanceHintManager* manager = getManager();
        int32_t tid = gettid();
        if (manager) perfHint->session = createSession(manager, &tid, 1, targetWorkDurationNanos);
    }

    println("vortek: perf hints: priority %s, ADPF %s (target %lld us)", perfHint->priorityRaised ? "raised" : "default",
            perfHint->session ? "active" : "unavailable", (long long)(targetWorkDurationNanos / 1000));
}

static inline void PerfHint_report(PerfHint* perfHint, int64_t actualWorkDurationNanos) {
    if (perfHint->session && actualWorkDurationNanos > 0) perfHint->reportActualWorkDuration(perfHint->session, actualWorkDurationNanos);
}

static inline void PerfHint_close(PerfHint* perfHint) {
    if (perfHint->session) perfHint->closeSession(perfHint->session);
    perfHint->session = NULL;
    if (perfHint->lib) dlclose(perfHint->lib);
    perfHint->lib = NULL;
}

#endif
