#ifndef VORTEK_FENCE_EXPORT_H
#define VORTEK_FENCE_EXPORT_H

#include "vortek.h"

/*
 * LURVETA Performance — correct fence semantics for client-side waits.
 *
 * vkWaitForFences with a timeout is implemented by exporting each fence as a SYNC_FD and letting the
 * client poll() it. Per the Vulkan spec, exporting a fence payload with copy transference (SYNC_FD)
 * "has the same side effects on the source fence's payload as executing a fence reset operation":
 * after the first wait the server-side fence is UNSIGNALED with no pending signal operation. A second
 * wait (or vkGetFenceStatus) on that fence then fails / reports NOT_READY, and the Vortek client turns
 * any failure of this path into VK_ERROR_DEVICE_LOST (seen as "DxvkSubmissionQueue: Failed to sync
 * fence: VK_ERROR_DEVICE_LOST" with GTA V, while the GPU was never lost).
 *
 * FenceExport keeps the truth on the server:
 *  - a fence that is already signaled is never exported: the client gets an eventfd that poll()
 *    reports readable at once;
 *  - a pending fence is exported ONCE; a duplicate of its sync fd is kept, and later waits/status
 *    queries use it instead of the (reset) driver fence;
 *  - vkResetFences, a new submission signaling the fence, an import or vkDestroyFence forget it.
 */
typedef struct FenceExport {
    SparseArray64 fds; /* VkFence -> (intptr_t)(sync fd + 1) */
} FenceExport;

/* An fd the client can poll for this fence (caller owns it), or -1 and *outResult set to the error. */
extern int FenceExport_waitFd(FenceExport* fenceExport, VkDevice device, VkFence fence, VkResult* outResult);
/* vkGetFenceStatus with the exported state taken into account. */
extern VkResult FenceExport_status(FenceExport* fenceExport, VkDevice device, VkFence fence);
/* The fence gets a new payload (reset, new signal operation, import) or is destroyed. */
extern void FenceExport_forget(FenceExport* fenceExport, VkFence fence);
extern void FenceExport_free(FenceExport* fenceExport);

#endif
