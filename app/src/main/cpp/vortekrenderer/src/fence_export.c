#include <poll.h>
#include <sys/eventfd.h>
#include <fcntl.h>

#include "fence_export.h"
#include "vulkan_helper.h"

static int storedFd(FenceExport* fenceExport, VkFence fence) {
    intptr_t value = (intptr_t)SparseArray64_get(&fenceExport->fds, (int64_t)fence);
    return value > 0 ? (int)(value - 1) : -1;
}

static bool fdSignaled(int fd) {
    struct pollfd pfd = {fd, POLLIN, 0};
    return poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0;
}

/* An fd that poll() reports readable immediately (the fence is already signaled). */
static int signaledFd() {
    return eventfd(1, EFD_CLOEXEC);
}

int FenceExport_waitFd(FenceExport* fenceExport, VkDevice device, VkFence fence, VkResult* outResult) {
    *outResult = VK_SUCCESS;

    int stored = storedFd(fenceExport, fence);
    if (stored >= 0) {
        int fd = fcntl(stored, F_DUPFD_CLOEXEC, 0);
        if (fd < 0) *outResult = VK_ERROR_OUT_OF_HOST_MEMORY;
        return fd;
    }

    VkResult status = vulkanWrapper.vkGetFenceStatus(device, fence);
    if (status == VK_SUCCESS) {
        // Signaled: do NOT export (that would reset the fence for every later query).
        int fd = signaledFd();
        if (fd < 0) *outResult = VK_ERROR_OUT_OF_HOST_MEMORY;
        return fd;
    }
    if (status < 0) {
        *outResult = status;
        return -1;
    }

    VkFenceGetFdInfoKHR getFdInfo = {0};
    getFdInfo.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR;
    getFdInfo.fence = fence;
    getFdInfo.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;

    int fd = -1;
    VkResult result = vulkanWrapper.vkGetFenceFd(device, &getFdInfo, &fd);
    if (result != VK_SUCCESS) {
        *outResult = result;
        return -1;
    }
    if (fd < 0) {
        // The spec allows -1 for "already signaled": hand out a readable fd instead (never -1 over SCM_RIGHTS).
        fd = signaledFd();
        if (fd < 0) *outResult = VK_ERROR_OUT_OF_HOST_MEMORY;
        return fd;
    }

    // The export reset the driver fence: keep the sync fd as the fence's real state.
    int keep = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (keep >= 0) SparseArray64_put(&fenceExport->fds, (int64_t)fence, (void*)(intptr_t)(keep + 1));
    return fd;
}

VkResult FenceExport_status(FenceExport* fenceExport, VkDevice device, VkFence fence) {
    int stored = storedFd(fenceExport, fence);
    if (stored >= 0) return fdSignaled(stored) ? VK_SUCCESS : VK_NOT_READY;
    return vulkanWrapper.vkGetFenceStatus(device, fence);
}

void FenceExport_forget(FenceExport* fenceExport, VkFence fence) {
    if (!fence) return;
    int index = SparseArray64_indexOfKey(&fenceExport->fds, (int64_t)fence);
    if (index < 0) return;
    intptr_t value = (intptr_t)fenceExport->fds.entries[index].value;
    if (value > 0) close((int)(value - 1));
    SparseArray64_removeAt(&fenceExport->fds, index);
}

void FenceExport_free(FenceExport* fenceExport) {
    for (int i = 0; i < fenceExport->fds.size; i++) {
        intptr_t value = (intptr_t)fenceExport->fds.entries[i].value;
        if (value > 0) close((int)(value - 1));
    }
    SparseArray64_free(&fenceExport->fds, false);
}
