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
 *
 * Diagnostics: every other way these calls can make the client report an error is logged as
 * "LURVETA vtio: ANOMALY ..." (poll/recvmsg/read failing with another errno, end of stream, an
 * fd reply that arrived without its fds or with truncated control data).
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

static volatile unsigned int anomalies = 0;

static void noteAnomaly(const char* call, const char* what, long result, int error) {
    unsigned int n = __atomic_add_fetch(&anomalies, 1, __ATOMIC_RELAXED);
    if (n <= 20) {
        fprintf(stderr, "LURVETA vtio: ANOMALY %s: %s (result %ld, errno %d %s) in pid %d, anomaly #%u\n",
                call, what, result, error, error ? strerror(error) : "", (int)getpid(), n);
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
        if (result < 0 && errno != EINTR) { int e = errno; noteAnomaly("poll (vkWaitForFences)", "failed", result, e); errno = e; }
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
    size_t controlRequested = msg->msg_controllen;
    for (;;) {
        msg->msg_controllen = controlRequested;
        ssize_t result = recvmsg(fd, msg, flags);
        int e = errno;
        if (result == 0) noteAnomaly("recvmsg (fd reply)", "end of stream (server closed the socket)", result, 0);
        else if (result < 0 && e != EINTR) noteAnomaly("recvmsg (fd reply)", "failed", result, e);
        else if (result > 0 && controlRequested > 0) {
            if (msg->msg_flags & MSG_CTRUNC) noteAnomaly("recvmsg (fd reply)", "fds dropped: control data truncated (fd limit?)", result, 0);
            else if (msg->msg_controllen == 0) noteAnomaly("recvmsg (fd reply)", "reply arrived without any fd", result, 0);
        }
        errno = e;
        if (result >= 0 || errno != EINTR) return result;
        noteEintr("recvmsg (fd reply)", &eintrRecvmsg);
        if (!retryEnabled) return result;
    }
}

ssize_t lurveta_read(int fd, void* buffer, size_t count) {
    for (;;) {
        ssize_t result = read(fd, buffer, count);
        int e = errno;
        if (result == 0) noteAnomaly("read", "end of file (server side closed without an answer)", result, 0);
        else if (result < 0 && e != EINTR) noteAnomaly("read", "failed", result, e);
        errno = e;
        if (result >= 0 || errno != EINTR) return result;
        noteEintr("read", &eintrRead);
        if (!retryEnabled) return result;
    }
}
