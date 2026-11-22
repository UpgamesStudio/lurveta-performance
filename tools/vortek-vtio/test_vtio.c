/* Host test (aarch64 under qemu): a library importing __poll_chk/recvmsg/read exactly like the
 * Vortek client is patched by the same patchelf steps; signals without SA_RESTART are fired at
 * the blocked thread. Expect: no EINTR leaks, timeouts still honoured, data still delivered. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

typedef int (*waitfn)(int fd, int timeoutMs);
typedef long (*recvfn)(int fd, char* buf, int len);
static void onSignal(int sig) { (void)sig; }
static pthread_t target;
static volatile int stop = 0;
static void* pester(void* arg) { (void)arg; while (!stop) { pthread_kill(target, SIGUSR1); usleep(2000); } return NULL; }
static long long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec*1000LL + t.tv_nsec/1000000; }
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } else { printf("ok: " __VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = onSignal; sa.sa_flags = 0; /* no SA_RESTART */
    sigaction(SIGUSR1, &sa, NULL);
    target = pthread_self();

    void* client = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    CHECK(client != NULL, "patched libvulkan_vortek.so loads with RTLD_NOW (%s)", client ? "" : dlerror());
    if (client) CHECK(dlsym(client, "vk_icdGetInstanceProcAddr") || dlsym(client, "vkGetInstanceProcAddr"), "client exports its ICD entry point");

    void* lib = dlopen(argv[2], RTLD_NOW);
    CHECK(lib != NULL, "patched test library loads (%s)", lib ? "" : dlerror());
    if (!lib) return 1;
    waitfn waitFd = (waitfn)dlsym(lib, "test_wait");
    recvfn recvData = (recvfn)dlsym(lib, "test_recv");

    pthread_t th; pthread_create(&th, NULL, pester, NULL);

    int efd = eventfd(0, 0);
    long long t0 = ms(); int r = waitFd(efd, 300); long long dt = ms() - t0;
    CHECK(r == 0 && dt >= 290 && dt < 1000, "poll with 300 ms timeout under signals -> timeout after %lld ms (r=%d)", dt, r);

    int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    if (fork() == 0) { usleep(200000); write(sv[1], "hello", 5); _exit(0); }
    char buf[16] = {0}; long n = recvData(sv[0], buf, sizeof buf);
    CHECK(n == 5 && memcmp(buf, "hello", 5) == 0, "recvmsg blocked 200 ms under signals still gets the data (n=%ld)", n);

    if (fork() == 0) { usleep(200000); unsigned long long one = 1; write(efd, &one, 8); _exit(0); }
    t0 = ms(); r = waitFd(efd, -1); dt = ms() - t0;
    CHECK(r == 1, "infinite poll under signals returns when the fd is signaled (r=%d, %lld ms)", r, dt);

    stop = 1; pthread_join(th, NULL);
    printf(failures ? "VTIO TEST FAILED (%d)\n" : "VTIO TEST PASSED\n", failures);
    return failures != 0;
}
