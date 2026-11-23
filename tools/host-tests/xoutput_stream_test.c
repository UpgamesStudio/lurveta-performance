/* Host test for the non-blocking X output stream (xoutput_stream.c).
 * A client that does not read must not block the X server thread; once it reads, it must get
 * every byte in order (and an fd sent with a message attached to that message). */
#define _GNU_SOURCE
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

jlong Java_com_winlator_xconnector_XOutputStream_nativeAllocate(JNIEnv* env, jobject obj, jint fd, jint initialCapacity);
void Java_com_winlator_xconnector_XOutputStream_writeInt(jlong ptr, jint value);
void Java_com_winlator_xconnector_XOutputStream_setAncillaryFd(jlong ptr, jint fd);
jboolean Java_com_winlator_xconnector_XOutputStream_sendData(jlong ptr);
void Java_com_winlator_xconnector_XOutputStream_destroy(jlong ptr);

static long long nowMs(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
static int failures = 0;
#define CHECK(c, ...) do { int ok_ = (c); printf("%s: ", ok_ ? "ok" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) failures++; } while (0)

#define MESSAGES 20000
#define INTS_PER_MESSAGE 64   /* 256 bytes per message, ~5 MB total */
#define FD_MESSAGE 15000

int main(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    int small = 16 * 1024;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    jlong out = Java_com_winlator_xconnector_XOutputStream_nativeAllocate(NULL, NULL, sv[0], 4096);

    /* 1. the client does not read: every send must return at once */
    long long worst = 0, start = nowMs();
    int sendFailures = 0, counter = 0;
    int efd = eventfd(7, 0);
    for (int m = 0; m < MESSAGES; m++) {
        for (int i = 0; i < INTS_PER_MESSAGE; i++) Java_com_winlator_xconnector_XOutputStream_writeInt(out, counter++);
        if (m == FD_MESSAGE) Java_com_winlator_xconnector_XOutputStream_setAncillaryFd(out, efd);
        long long t0 = nowMs();
        if (!Java_com_winlator_xconnector_XOutputStream_sendData(out)) sendFailures++;
        long long dt = nowMs() - t0;
        if (dt > worst) worst = dt;
    }
    close(efd);   /* the caller may close its fd right after sending */
    CHECK(sendFailures == 0, "%d messages (~5 MB) to a client that does not read: no send failed", MESSAGES);
    CHECK(worst < 100, "the X server thread never blocked (slowest send %lld ms, total %lld ms)", worst, nowMs() - start);

    /* 2. the client reads now: every value in order, and the fd arrives */
    int expected = 0, gotFd = -1, badOrder = 0;
    long long deadline = nowMs() + 10000;
    while (expected < counter && nowMs() < deadline) {
        int values[1024];
        union { struct cmsghdr align; char buf[CMSG_SPACE(sizeof(int) * 4)]; } control;
        struct iovec iov = {values, sizeof(values)};
        struct msghdr msg = {0};
        msg.msg_iov = &iov; msg.msg_iovlen = 1; msg.msg_control = control.buf; msg.msg_controllen = sizeof(control.buf);
        ssize_t n = recvmsg(sv[1], &msg, 0);
        if (n <= 0) break;
        for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
            if (c->cmsg_type == SCM_RIGHTS) memcpy(&gotFd, CMSG_DATA(c), sizeof(int));
        if (n % 4) { /* keep alignment simple: read the rest of the int */
            int extra = 4 - (int)(n % 4); char* p = (char*)values + n;
            while (extra > 0) { ssize_t r = read(sv[1], p, extra); if (r <= 0) break; p += r; extra -= (int)r; n += r; }
        }
        for (int i = 0; i < n / 4; i++) if (values[i] != expected++) badOrder++;
    }
    CHECK(expected == counter && badOrder == 0, "client received all %d values in order (%d out of order)", expected, badOrder);
    uint64_t v = 0;
    CHECK(gotFd >= 0 && read(gotFd, &v, sizeof(v)) == sizeof(v) && v == 7, "fd sent with a queued message arrived and is usable");

    /* 3. destroying a stream with queued data is safe */
    for (int i = 0; i < 100000; i++) Java_com_winlator_xconnector_XOutputStream_writeInt(out, i);
    Java_com_winlator_xconnector_XOutputStream_sendData(out);
    Java_com_winlator_xconnector_XOutputStream_destroy(out);
    usleep(100000);
    CHECK(1, "stream with queued data destroyed without crashing");

    printf(failures ? "XOUTPUT STREAM TESTS FAILED (%d)\n" : "ALL XOUTPUT STREAM TESTS PASSED\n", failures);
    return failures != 0;
}
