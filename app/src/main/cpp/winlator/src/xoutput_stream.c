#include <malloc.h>
#include <string.h>
#include <jni.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include "winlator.h"
#include "socket_utils.h"

/*
 * LURVETA: the X server writes to its clients WITHOUT blocking.
 *
 * Upstream wrote each reply/event with a blocking write() on the X server's single epoll thread
 * while holding the X server lock. When a client did not read for a while (GTA V during its intro
 * sent a burst of ConfigureWindow requests and received a ConfigureNotify for each), its socket
 * buffer filled up, write() blocked, the whole X server stopped (frozen image) and the UI thread
 * waiting for the X server lock was killed by Android (ANR). A short write also silently dropped
 * the rest of the message.
 *
 * Now: send with MSG_DONTWAIT; whatever the socket does not take is queued per client, in order,
 * and a flusher thread writes it as soon as poll() says the socket is writable. New data for a
 * client that still has queued data is appended behind it, so the byte stream stays intact.
 */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/stat.h>

#define PENDING_LIMIT (256 * 1024 * 1024) /* a client that stopped reading entirely: give up */

typedef struct PendingChunk {
    struct PendingChunk* next;
    int fd;        /* ancillary fd sent with the first byte of this chunk, or -1 */
    int offset;
    int length;
    char data[];
} PendingChunk;

typedef struct XOutputStream {
    int fd;
    int ancillaryFd;
    struct {
        void* data;
        int capacity;
        int position;
        int limit;
    } buffer;
    /* LURVETA: unsent bytes, oldest first (guarded by mutex) */
    pthread_mutex_t mutex;
    PendingChunk* pendingHead;
    PendingChunk* pendingTail;
    size_t pendingBytes;
    bool failed;
    dev_t dev;
    ino_t ino;
    struct XOutputStream* nextRegistered;
} XOutputStream;

static pthread_mutex_t registryMutex = PTHREAD_MUTEX_INITIALIZER;
static XOutputStream* registry = NULL;
static int wakeFd = -1;
static bool flusherStarted = false;

static ssize_t sendNonBlocking(int fd, const void* data, int length, int ancillaryFd) {
    struct iovec iov = {.iov_base = (void*)data, .iov_len = length};
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    union { struct cmsghdr align; char buf[CMSG_SPACE(sizeof(int))]; } control;
    if (ancillaryFd >= 0) {
        memset(&control, 0, sizeof(control));
        msg.msg_control = control.buf;
        msg.msg_controllen = CMSG_SPACE(sizeof(int));
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &ancillaryFd, sizeof(int));
    }
    ssize_t sent;
    do sent = sendmsg(fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
    while (sent < 0 && errno == EINTR);
    return sent;
}

/* Writes queued chunks until the socket is full. Called with outputStream->mutex held. */
static void flushPending(XOutputStream* outputStream) {
    while (outputStream->pendingHead && !outputStream->failed) {
        PendingChunk* chunk = outputStream->pendingHead;
        ssize_t sent = sendNonBlocking(outputStream->fd, chunk->data + chunk->offset, chunk->length - chunk->offset, chunk->offset == 0 ? chunk->fd : -1);
        if (sent < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) outputStream->failed = true;
            return;
        }
        if (sent > 0 && chunk->offset == 0 && chunk->fd >= 0) { close(chunk->fd); chunk->fd = -1; }
        chunk->offset += (int)sent;
        outputStream->pendingBytes -= sent;
        if (chunk->offset < chunk->length) return;
        outputStream->pendingHead = chunk->next;
        if (!chunk->next) outputStream->pendingTail = NULL;
        free(chunk);
    }
}

static void freePending(XOutputStream* outputStream) {
    while (outputStream->pendingHead) {
        PendingChunk* chunk = outputStream->pendingHead;
        outputStream->pendingHead = chunk->next;
        if (chunk->fd >= 0) close(chunk->fd);
        free(chunk);
    }
    outputStream->pendingTail = NULL;
    outputStream->pendingBytes = 0;
}

/* True while the fd still refers to the socket this stream was created for (fds are reused). */
static bool sameSocket(XOutputStream* outputStream) {
    struct stat st;
    return fstat(outputStream->fd, &st) == 0 && st.st_dev == outputStream->dev && st.st_ino == outputStream->ino;
}

static void* flusherThread(void* arg) {
    (void)arg;
    struct pollfd fds[256];
    XOutputStream* streams[256];
    for (;;) {
        int count = 0;
        fds[count].fd = wakeFd;
        fds[count].events = POLLIN;
        streams[count++] = NULL;
        pthread_mutex_lock(&registryMutex);
        for (XOutputStream* s = registry; s && count < 256; s = s->nextRegistered) {
            pthread_mutex_lock(&s->mutex);
            bool waiting = s->pendingHead != NULL && !s->failed;
            pthread_mutex_unlock(&s->mutex);
            if (!waiting) continue;
            fds[count].fd = s->fd;
            fds[count].events = POLLOUT;
            streams[count++] = s;
        }
        pthread_mutex_unlock(&registryMutex);

        if (poll(fds, count, count > 1 ? 50 : -1) < 0 && errno != EINTR) usleep(10000);
        if (fds[0].revents & POLLIN) {
            uint64_t value;
            if (read(wakeFd, &value, sizeof(value)) < 0) {}
        }

        pthread_mutex_lock(&registryMutex);
        for (int i = 1; i < count; i++) {
            if (!(fds[i].revents & (POLLOUT | POLLERR | POLLHUP))) continue;
            /* the stream may have been destroyed meanwhile: only touch it if still registered */
            for (XOutputStream* s = registry; s; s = s->nextRegistered) {
                if (s != streams[i]) continue;
                pthread_mutex_lock(&s->mutex);
                if (!sameSocket(s)) { s->failed = true; freePending(s); }
                else flushPending(s);
                pthread_mutex_unlock(&s->mutex);
                break;
            }
        }
        pthread_mutex_unlock(&registryMutex);
    }
    return NULL;
}

static void wakeFlusher(void) {
    uint64_t one = 1;
    if (wakeFd >= 0 && write(wakeFd, &one, sizeof(one)) < 0) {}
}

static void registerStream(XOutputStream* outputStream) {
    pthread_mutex_lock(&registryMutex);
    if (!flusherStarted) {
        wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        pthread_t thread;
        flusherStarted = pthread_create(&thread, NULL, flusherThread, NULL) == 0;
        if (flusherStarted) pthread_detach(thread);
    }
    outputStream->nextRegistered = registry;
    registry = outputStream;
    pthread_mutex_unlock(&registryMutex);
}

static void unregisterStream(XOutputStream* outputStream) {
    pthread_mutex_lock(&registryMutex);
    for (XOutputStream** p = &registry; *p; p = &(*p)->nextRegistered) {
        if (*p == outputStream) { *p = outputStream->nextRegistered; break; }
    }
    pthread_mutex_unlock(&registryMutex);
}

static void ensureSpaceIsAvailable(XOutputStream* outputStream, int length) {
    if ((outputStream->buffer.capacity - outputStream->buffer.position) >= length) return;

    int newCapacity = outputStream->buffer.capacity + length;
    void* newData = realloc(outputStream->buffer.data, newCapacity);
    outputStream->buffer.capacity = newCapacity;
    outputStream->buffer.limit = newCapacity;
    outputStream->buffer.data = newData;
}

static XOutputStream* XOutputStream_allocate(int fd, int initialCapacity) {
    XOutputStream* outputStream = calloc(1, sizeof(XOutputStream));
    outputStream->fd = fd;
    outputStream->buffer.data = calloc(initialCapacity, 1);
    outputStream->buffer.capacity = initialCapacity;
    outputStream->buffer.limit = initialCapacity;
    pthread_mutex_init(&outputStream->mutex, NULL);
    struct stat st;
    if (fstat(fd, &st) == 0) { outputStream->dev = st.st_dev; outputStream->ino = st.st_ino; }
    registerStream(outputStream);
    return outputStream;
}

static void XOutputStream_destroy(XOutputStream* outputStream) {
    if (!outputStream) return;
    unregisterStream(outputStream);
    pthread_mutex_lock(&outputStream->mutex);
    freePending(outputStream);
    pthread_mutex_unlock(&outputStream->mutex);
    pthread_mutex_destroy(&outputStream->mutex);
    MEMFREE(outputStream->buffer.data);
    MEMFREE(outputStream);
}

static jboolean XOutputStream_send(XOutputStream* outputStream) {
    int length = outputStream->buffer.position;
    int ancillaryFd = outputStream->ancillaryFd > 0 ? outputStream->ancillaryFd : -1;
    outputStream->ancillaryFd = 0;
    outputStream->buffer.position = 0;
    outputStream->buffer.limit = outputStream->buffer.capacity;
    if (length == 0) return JNI_TRUE;

    pthread_mutex_lock(&outputStream->mutex);
    bool queued = false;
    int sentNow = 0;
    if (!outputStream->failed && !outputStream->pendingHead) {
        ssize_t sent = sendNonBlocking(outputStream->fd, outputStream->buffer.data, length, ancillaryFd);
        if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) outputStream->failed = true;
        else if (sent > 0) sentNow = (int)sent;
    }
    if (!outputStream->failed && sentNow < length) {
        if (outputStream->pendingBytes + (length - sentNow) > PENDING_LIMIT) outputStream->failed = true;
        else {
            PendingChunk* chunk = malloc(sizeof(PendingChunk) + (length - sentNow));
            if (!chunk) outputStream->failed = true;
            else {
                chunk->next = NULL;
                chunk->offset = 0;
                chunk->length = length - sentNow;
                /* the fd travels with the first byte: if any byte was sent, it already went out */
                chunk->fd = sentNow == 0 && ancillaryFd >= 0 ? fcntl(ancillaryFd, F_DUPFD_CLOEXEC, 0) : -1;
                memcpy(chunk->data, (char*)outputStream->buffer.data + sentNow, chunk->length);
                if (outputStream->pendingTail) outputStream->pendingTail->next = chunk;
                else outputStream->pendingHead = chunk;
                outputStream->pendingTail = chunk;
                outputStream->pendingBytes += chunk->length;
                queued = true;
            }
        }
    }
    bool failed = outputStream->failed;
    pthread_mutex_unlock(&outputStream->mutex);
    if (queued) wakeFlusher();
    return failed ? JNI_FALSE : JNI_TRUE;
}

JNIEXPORT jlong JNICALL
Java_com_winlator_xconnector_XOutputStream_nativeAllocate(JNIEnv *env, jobject obj, jint fd,
                                                          jint initialCapacity) {
    XOutputStream* outputStream = XOutputStream_allocate(fd, initialCapacity);
    return (jlong)outputStream;
}

JNIEXPORT void JNICALL
Java_com_winlator_xconnector_XOutputStream_setAncillaryFd(jlong nativePtr, jint ancillaryFd) {
    ((XOutputStream*)nativePtr)->ancillaryFd = ancillaryFd;
}

JNIEXPORT void JNICALL
Java_com_winlator_xconnector_XOutputStream_writeByte(jlong nativePtr, jbyte value) {
    XOutputStream* outputStream = (XOutputStream*)nativePtr;
    ensureSpaceIsAvailable(outputStream, 1);
    *(jbyte*)(outputStream->buffer.data + outputStream->buffer.position++) = value;
}

JNIEXPORT void JNICALL
Java_com_winlator_xconnector_XOutputStream_writeShort(jlong nativePtr, jshort value) {
    XOutputStream* outputStream = (XOutputStream*)nativePtr;
    ensureSpaceIsAvailable(outputStream, 2);
    *(jshort*)(outputStream->buffer.data + outputStream->buffer.position) = value;
    outputStream->buffer.position += 2;
}

JNIEXPORT void JNICALL
Java_com_winlator_xconnector_XOutputStream_writeInt(jlong nativePtr, jint value) {
    XOutputStream* outputStream = (XOutputStream*)nativePtr;
    ensureSpaceIsAvailable(outputStream, 4);
    *(jint*)(outputStream->buffer.data + outputStream->buffer.position) = value;
    outputStream->buffer.position += 4;
}

JNIEXPORT void JNICALL
Java_com_winlator_xconnector_XOutputStream_writeLong(jlong nativePtr, jlong value) {
    XOutputStream* outputStream = (XOutputStream*)nativePtr;
    ensureSpaceIsAvailable(outputStream, 8);
    *(jlong*)(outputStream->buffer.data + outputStream->buffer.position) = value;
    outputStream->buffer.position += 8;
}

JNIEXPORT void JNICALL
Java_com_winlator_xconnector_XOutputStream_writePad(jlong nativePtr, jint length) {
    XOutputStream* outputStream = (XOutputStream*)nativePtr;
    ensureSpaceIsAvailable(outputStream, length);
    memset(outputStream->buffer.data + outputStream->buffer.position, 0, length);
    outputStream->buffer.position += length;
}

JNIEXPORT void JNICALL
Java_com_winlator_xconnector_XOutputStream_writeAt(JNIEnv *env, jclass obj,
                                                   jlong nativePtr, jint position, jbyteArray data) {
    XOutputStream* outputStream = (XOutputStream*)nativePtr;
    jbyte* dataPtr = (*env)->GetByteArrayElements(env, data, 0);
    jsize length = (*env)->GetArrayLength(env, data);
    memcpy(outputStream->buffer.data + position, dataPtr, length);
    (*env)->ReleaseByteArrayElements(env, data, dataPtr, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_winlator_xconnector_XOutputStream_writeByteBuffer(JNIEnv *env, jclass obj,
                                                           jlong nativePtr, jobject data,
                                                           jint offset, jint length) {
    XOutputStream* outputStream = (XOutputStream*)nativePtr;
    void* dataAddr = (*env)->GetDirectBufferAddress(env, data);
    ensureSpaceIsAvailable(outputStream, length);
    memcpy(outputStream->buffer.data + outputStream->buffer.position, dataAddr + offset, length);
    outputStream->buffer.position += length;
}

JNIEXPORT jboolean JNICALL
Java_com_winlator_xconnector_XOutputStream_sendData(jlong nativePtr) {
    return XOutputStream_send((XOutputStream*)nativePtr);
}

JNIEXPORT void JNICALL
Java_com_winlator_xconnector_XOutputStream_destroy(jlong nativePtr) {
    XOutputStream_destroy((XOutputStream*)nativePtr);
}

JNIEXPORT jint JNICALL
Java_com_winlator_xconnector_XOutputStream_length(jlong nativePtr) {
    XOutputStream* outputStream = (XOutputStream*)nativePtr;
    return outputStream->buffer.position;
}