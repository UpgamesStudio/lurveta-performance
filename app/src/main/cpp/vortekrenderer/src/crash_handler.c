#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

#include "crash_handler.h"

#ifndef APP_ID
#define APP_ID "com.lurveta.co"
#endif
#ifndef LURVETA_BUILD_ID
#define LURVETA_BUILD_ID "unknown"
#endif
#define CRASH_DIR "/data/data/" APP_ID "/files/logs"
#define CRASH_FILE CRASH_DIR "/native-crash.txt"
#define ALT_STACK_SIZE (64 * 1024)

static const int crashSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};
#define SIGNAL_COUNT (int)(sizeof(crashSignals) / sizeof(crashSignals[0]))
static struct sigaction previousActions[SIGNAL_COUNT];
static volatile int installed = 0;
static volatile int handling = 0;
static __thread int currentRequest = -1;
static __thread int currentClientPid = 0;
static __thread int hasAltStack = 0;
static const char* (*requestName)(short) = NULL;

void CrashHandler_setRequestNames(const char* (*lookup)(short)) { requestName = lookup; }

void CrashHandler_setActivity(int requestCode, int clientPid) {
    currentRequest = requestCode;
    currentClientPid = clientPid;
}

void CrashHandler_threadInit(void) {
    if (hasAltStack) return;
    void* stack = mmap(NULL, ALT_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) return;
    stack_t ss = {.ss_sp = stack, .ss_size = ALT_STACK_SIZE, .ss_flags = 0};
    if (sigaltstack(&ss, NULL) == 0) hasAltStack = 1;
    else munmap(stack, ALT_STACK_SIZE);
}

static void writeText(int fd, const char* text) {
    size_t length = strlen(text);
    while (length > 0) {
        ssize_t written = write(fd, text, length);
        if (written <= 0) return;
        text += written;
        length -= (size_t)written;
    }
}

static void writeAddress(int fd, const char* label, uintptr_t address) {
    char line[512];
    Dl_info info;
    if (address && dladdr((void*)address, &info) && info.dli_fname) {
        const char* lib = strrchr(info.dli_fname, '/');
        lib = lib ? lib + 1 : info.dli_fname;
        if (info.dli_sname) {
            snprintf(line, sizeof(line), "%s 0x%lx %s (%s+0x%lx) [offset 0x%lx]\n", label, (unsigned long)address, lib, info.dli_sname,
                     (unsigned long)(address - (uintptr_t)info.dli_saddr), (unsigned long)(address - (uintptr_t)info.dli_fbase));
        }
        else {
            snprintf(line, sizeof(line), "%s 0x%lx %s [offset 0x%lx]\n", label, (unsigned long)address, lib, (unsigned long)(address - (uintptr_t)info.dli_fbase));
        }
    }
    else snprintf(line, sizeof(line), "%s 0x%lx ?\n", label, (unsigned long)address);
    writeText(fd, line);
}

typedef struct BacktraceState { int fd; int frames; } BacktraceState;

static _Unwind_Reason_Code backtraceFrame(struct _Unwind_Context* context, void* arg) {
    BacktraceState* state = arg;
    uintptr_t pc = _Unwind_GetIP(context);
    if (!pc) return _URC_END_OF_STACK;
    char label[24];
    snprintf(label, sizeof(label), "  #%02d", state->frames);
    writeAddress(state->fd, label, pc);
    return ++state->frames >= 48 ? _URC_END_OF_STACK : _URC_NO_REASON;
}

static const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (invalid memory access)";
        case SIGBUS: return "SIGBUS (bad memory access)";
        case SIGILL: return "SIGILL (illegal instruction)";
        case SIGFPE: return "SIGFPE (arithmetic error)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGTRAP: return "SIGTRAP (trap)";
        default: return "signal";
    }
}

static void crashHandler(int sig, siginfo_t* info, void* ucontextPtr) {
    int index = 0;
    while (index < SIGNAL_COUNT && crashSignals[index] != sig) index++;

    if (!__atomic_exchange_n(&handling, 1, __ATOMIC_SEQ_CST)) {
        mkdir(CRASH_DIR, 0771);
        int fd = open(CRASH_FILE, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0660);
        if (fd >= 0) {
            char line[512];
            char threadName[32] = {0};
            prctl(PR_GET_NAME, threadName, 0, 0, 0);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            snprintf(line, sizeof(line), "=== NATIVE CRASH in the app process (epoch %lld) | build %s ===\n%s, code %d, fault address %p\nthread '%s' (tid %ld, pid %d)\n",
                     (long long)ts.tv_sec, LURVETA_BUILD_ID, signalName(sig), info ? info->si_code : 0, info ? info->si_addr : NULL,
                     threadName, (long)syscall(SYS_gettid), (int)getpid());
            writeText(fd, line);
            if (currentRequest >= 0) {
                snprintf(line, sizeof(line), "Vortek server was handling %s for client pid %d\n", requestName ? requestName((short)currentRequest) : "request", currentClientPid);
                writeText(fd, line);
            }
#if defined(__aarch64__)
            if (ucontextPtr) {
                ucontext_t* uc = ucontextPtr;
                writeAddress(fd, "pc", (uintptr_t)uc->uc_mcontext.pc);
                writeAddress(fd, "lr", (uintptr_t)uc->uc_mcontext.regs[30]);
                snprintf(line, sizeof(line), "sp 0x%lx\n", (unsigned long)uc->uc_mcontext.sp);
                writeText(fd, line);
            }
#endif
            writeText(fd, "backtrace:\n");
            BacktraceState state = {fd, 0};
            _Unwind_Backtrace(backtraceFrame, &state);
            writeText(fd, "=== end ===\n");
            close(fd);
        }
    }

    // Let Android's own crash handling (debuggerd, tombstone) run as it did before.
    if (index < SIGNAL_COUNT) sigaction(sig, &previousActions[index], NULL);
    else signal(sig, SIG_DFL);
    if (!info || info->si_code <= 0) syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), sig);  // raised signal: send it again
    // A fault re-executes the faulting instruction on return and reaches the previous handler.
}

void CrashHandler_install(void) {
    if (__atomic_exchange_n(&installed, 1, __ATOMIC_SEQ_CST)) return;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = crashHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int i = 0; i < SIGNAL_COUNT; i++) sigaction(crashSignals[i], &action, &previousActions[i]);
}
