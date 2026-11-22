/*
 * Host test for fence_export.c (the code shipped in the APK) against a mock driver that follows the
 * Vulkan spec for SYNC_FD export: "exporting a fence payload with copy transference has the same side
 * effects on the source fence's payload as executing a fence reset operation"; exporting a fence that
 * is unsignaled with no pending signal operation fails; a signaled fence may be exported as -1.
 *
 * It replays a DXVK-like sequence with the ORIGINAL Vortek algorithm (always export) and with
 * FenceExport, and checks what the Vortek client would see. The client (disassembled from
 * libvulkan_vortek.so) returns VK_ERROR_DEVICE_LOST when the server reports any error or sends no fd.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <errno.h>

#include "vulkan_helper.h"
#include "fence_export.h"
#include "socket_utils.h"

VulkanWrapper vulkanWrapper;
uint32_t getMemoryTypeIndex(uint32_t typeBits, VkFlags properties) { return 0; }
uint32_t getMemoryPropertyFlags(uint32_t memoryTypeIndex) { return 0; }

/* ---- mock driver ------------------------------------------------------------------------- */
enum { UNSIGNALED, PENDING, SIGNALED };
typedef struct MockFence { int state; int completion; int exports; } MockFence;
static int mockSignaledExportReturnsMinusOne = 1;

static VkResult VKAPI_CALL mockGetFenceStatus(VkDevice d, VkFence f) {
    MockFence* m = (MockFence*)f;
    return m->state == SIGNALED ? VK_SUCCESS : VK_NOT_READY;
}
static VkResult VKAPI_CALL mockGetFenceFd(VkDevice d, const VkFenceGetFdInfoKHR* info, int* fd) {
    MockFence* m = (MockFence*)info->fence;
    m->exports++;
    if (m->state == UNSIGNALED) return VK_ERROR_INVALID_EXTERNAL_HANDLE;   // nothing to export
    if (m->state == SIGNALED) {
        *fd = mockSignaledExportReturnsMinusOne ? -1 : eventfd(1, 0);
        m->state = UNSIGNALED;                                              // export = reset
        return VK_SUCCESS;
    }
    *fd = dup(m->completion);                                               // pending: sync fd
    m->state = UNSIGNALED;                                                  // export = reset
    return VK_SUCCESS;
}
static void submit(MockFence* m) { m->state = PENDING; uint64_t v; while (read(m->completion, &v, 8) > 0) {} }
static void gpuComplete(MockFence* m) { uint64_t one = 1; write(m->completion, &one, 8); if (m->state == PENDING) m->state = SIGNALED; }
static void resetFence(MockFence* m) { m->state = UNSIGNALED; uint64_t v; while (read(m->completion, &v, 8) > 0) {} }
static int readable(int fd) { struct pollfd p = {fd, POLLIN, 0}; return poll(&p, 1, 0) > 0; }

/* What the Vortek client returns for vkWaitForFences, given what the server sent. */
static const char* clientView(VkResult serverResult, int numFds, int fdReadable) {
    if (numFds == 0 || serverResult != VK_SUCCESS) return "VK_ERROR_DEVICE_LOST";
    return fdReadable ? "VK_SUCCESS" : "waits (fence not signaled yet)";
}

/* ---- the original server algorithm (always export) ------------------------------------------ */
static const char* originalWait(MockFence* m, int* sentFd) {
    int fd = -7;
    VkFenceGetFdInfoKHR info = {VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, NULL, (VkFence)m, VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
    VkResult r = vulkanWrapper.vkGetFenceFd(NULL, &info, &fd);
    *sentFd = fd;
    return clientView(r, 1, r == VK_SUCCESS && fd >= 0 && readable(fd));
}

static const char* newWait(FenceExport* fe, MockFence* m) {
    VkResult r;
    int fd = FenceExport_waitFd(fe, NULL, (VkFence)m, &r);
    const char* view = clientView(r, fd >= 0 ? 1 : 0, fd >= 0 && readable(fd));
    if (fd >= 0) close(fd);
    return view;
}

static int fails = 0;
#define EXPECT(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void) {
    vulkanWrapper.vkGetFenceStatus = mockGetFenceStatus;
    vulkanWrapper.vkGetFenceFd = mockGetFenceFd;

    /* 1. The kernel refuses -1 in SCM_RIGHTS: the original code then sent NOTHING (client hangs). */
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    int bad = -1; VkResult ok = VK_SUCCESS;
    int sent = send_fds(sv[0], &bad, 1, &ok, sizeof(ok));
    printf("send_fds with fd -1: %d (%s)\n", sent, sent < 0 ? strerror(errno) : "sent");
    EXPECT(sent < 0, "the kernel should reject fd -1 in SCM_RIGHTS");

    /* 2. DXVK-like sequence on one fence: submit, wait, GPU done, status, wait again. */
    MockFence a = {UNSIGNALED, eventfd(0, EFD_NONBLOCK), 0};
    int fd;
    submit(&a);
    const char* w1 = originalWait(&a, &fd);
    gpuComplete(&a);
    VkResult statusOld = vulkanWrapper.vkGetFenceStatus(NULL, (VkFence)&a);
    const char* w2 = originalWait(&a, &fd);
    printf("ORIGINAL: wait#1 -> %s | status after GPU done -> %s | wait#2 -> %s\n", w1, statusOld == VK_SUCCESS ? "VK_SUCCESS" : "VK_NOT_READY", w2);
    EXPECT(strcmp(w2, "VK_ERROR_DEVICE_LOST") == 0, "the original algorithm should reproduce the DEVICE_LOST (mechanism check)");

    FenceExport fe = {0};
    MockFence b = {UNSIGNALED, eventfd(0, EFD_NONBLOCK), 0};
    submit(&b);
    const char* n1 = newWait(&fe, &b);
    gpuComplete(&b);
    VkResult statusNew = FenceExport_status(&fe, NULL, (VkFence)&b);
    const char* n2 = newWait(&fe, &b);
    printf("NEW:      wait#1 -> %s | status after GPU done -> %s | wait#2 -> %s | driver exports: %d\n", n1,
           statusNew == VK_SUCCESS ? "VK_SUCCESS" : "VK_NOT_READY", n2, b.exports);
    EXPECT(statusNew == VK_SUCCESS, "status must be SUCCESS once the GPU finished");
    EXPECT(strcmp(n2, "VK_SUCCESS") == 0, "second wait must succeed");
    EXPECT(b.exports == 1, "a fence is exported once per payload");

    /* 3. Reset + resubmit: the kept fd is forgotten, the new payload is tracked again. */
    FenceExport_forget(&fe, (VkFence)&b);
    resetFence(&b);
    EXPECT(FenceExport_status(&fe, NULL, (VkFence)&b) == VK_NOT_READY, "after reset the fence is unsignaled");
    submit(&b);
    EXPECT(strcmp(newWait(&fe, &b), "waits (fence not signaled yet)") == 0, "pending fence: client waits");
    gpuComplete(&b);
    EXPECT(FenceExport_status(&fe, NULL, (VkFence)&b) == VK_SUCCESS, "resubmitted fence signals");

    /* 4. Already signaled when waited: never exported, client sees it ready. */
    MockFence c = {UNSIGNALED, eventfd(0, EFD_NONBLOCK), 0};
    submit(&c);
    gpuComplete(&c);
    const char* s1 = newWait(&fe, &c);
    const char* s2 = newWait(&fe, &c);
    printf("NEW, fence already signaled: wait -> %s, again -> %s, driver exports: %d, driver state still signaled: %d\n",
           s1, s2, c.exports, c.state == SIGNALED);
    EXPECT(strcmp(s1, "VK_SUCCESS") == 0 && strcmp(s2, "VK_SUCCESS") == 0 && c.exports == 0 && c.state == SIGNALED, "signaled fence handled without export");

    /* 5. Waiting on a fence that was never submitted is still reported (app error), not hidden. */
    MockFence d = {UNSIGNALED, eventfd(0, EFD_NONBLOCK), 0};
    VkResult r;
    int dfd = FenceExport_waitFd(&fe, NULL, (VkFence)&d, &r);
    EXPECT(dfd < 0 && r != VK_SUCCESS, "never-submitted fence reports the driver error");

    FenceExport_free(&fe);
    printf(fails ? "%d FAILURES\n" : "ALL FENCE EXPORT TESTS PASSED\n", fails);
    return fails != 0;
}
