#include <poll.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>
/* Mirrors the client, which calls glibc's fortified __poll_chk and plain recvmsg/read. */
extern int __poll_chk(struct pollfd* fds, nfds_t nfds, int timeout, size_t fdslen);
int test_wait(int fd, int timeoutMs) { struct pollfd p[1] = {{fd, POLLIN, 0}}; return __poll_chk(p, 1, timeoutMs, sizeof p); }
long test_recv(int fd, char* buf, int len) {
    struct iovec iov = {buf, (size_t)len}; struct msghdr m; memset(&m, 0, sizeof m); m.msg_iov = &iov; m.msg_iovlen = 1;
    return recvmsg(fd, &m, 0);
}
long test_read(int fd, char* buf, int len) { return read(fd, buf, len); }
