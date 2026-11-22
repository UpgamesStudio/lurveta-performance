/*
 * LURVETA vtio: EINTR-safe I/O for the prebuilt Vortek Vulkan client (libvulkan_vortek.so).
 *
 * The client blob treats any failure of poll()/recvmsg()/read() as fatal:
 *   - vkWaitForFences: poll() == -1  -> VK_ERROR_DEVICE_LOST
 *   - recv_fds (vkWaitForFences, vkMapMemory, ...): recvmsg() == -1 -> "0 fds" -> error, and the
 *     unread reply stays in the socket, so the next fd-carrying reply is read by the wrong call.
 * A signal delivered to the waiting thread (Wine uses signals to suspend threads and read their
 * context) makes these calls fail with EINTR, which is not an error at all.
 *
 * build.sh renames the client's imports __poll_chk/recvmsg/read to the lurveta_* names below and
 * adds this library as DT_NEEDED, so only the Vortek client is affected.
 *
 * LURVETA_VTIO_RETRY=0 disables the retries (A/B test); EINTR events are still counted and logged.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

static int retryEnabled = 1;
static volatile unsigned int eintrPoll = 0, eintrRecvmsg = 0, eintrRead = 0;

__attribute__((constructor)) static void vtioInit(void) {
    const char* value = getenv("LURVETA_VTIO_RETRY");
    if (value && strcmp(value, "0") == 0) retryEnabled = 0;
    fprintf(stderr, "LURVETA vtio: active in pid %d (EINTR retry %s)\n", (int)getpid(), retryEnabled ? "on" : "OFF");
}

static void noteEintr(const char* call, volatile unsigned int* counter) {
    unsigned int n = __atomic_add_fetch(counter, 1, __ATOMIC_RELAXED);
    // 1, 2, 4, 8, ...: enough to see that it happens and how often, without flooding the log.
    if ((n & (n - 1)) == 0) {
        fprintf(stderr, "LURVETA vtio: %s interrupted by a signal (EINTR) in pid %d, count %u, %s\n",
                call, (int)getpid(), n, retryEnabled ? "retried" : "NOT retried (client reports an error)");
    }
}

static long long nowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int lurveta_poll_chk(struct pollfd* fds, nfds_t nfds, int timeout, size_t fdslen) {
    if (fdslen / sizeof(*fds) < nfds) abort();   // what glibc's __poll_chk does
    long long deadline = timeout > 0 ? nowMs() + timeout : 0;
    for (;;) {
        int result = poll(fds, nfds, timeout);
        if (result >= 0 || errno != EINTR) return result;
        noteEintr("poll (vkWaitForFences)", &eintrPoll);
        if (!retryEnabled) return result;
        if (timeout > 0) {
            long long left = deadline - nowMs();
            if (left <= 0) return 0;              // the wait timed out while interrupted
            timeout = (int)left;
        }
    }
}

ssize_t lurveta_recvmsg(int fd, struct msghdr* msg, int flags) {
    for (;;) {
        ssize_t result = recvmsg(fd, msg, flags);
        if (result >= 0 || errno != EINTR) return result;
        noteEintr("recvmsg (fd reply)", &eintrRecvmsg);
        if (!retryEnabled) return result;
    }
}

ssize_t lurveta_read(int fd, void* buffer, size_t count) {
    for (;;) {
        ssize_t result = read(fd, buffer, count);
        if (result >= 0 || errno != EINTR) return result;
        noteEintr("read", &eintrRead);
        if (!retryEnabled) return result;
    }
}
