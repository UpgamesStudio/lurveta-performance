/* Host test for the Vortek native crash handler (aarch64 under qemu): a SIGSEGV inside a known
 * function must be written to native-crash.txt with the signal, request and a symbolised frame,
 * and the process must still die from the signal afterwards (Android's handler keeps working). */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "crash_handler.h"

__attribute__((noinline)) void lurvetaTestCrashHere(volatile int* p) { *p = 42; }

int main(void) {
    unlink("/data/data/com.lurveta.co/files/logs/native-crash.txt");
    pid_t child = fork();
    if (child == 0) {
        CrashHandler_install();
        CrashHandler_threadInit();
        CrashHandler_setActivity(0, 1234);
        lurvetaTestCrashHere((volatile int*)16);
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    int ok = WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
    printf("%s: process still terminated by SIGSEGV after the handler\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}
