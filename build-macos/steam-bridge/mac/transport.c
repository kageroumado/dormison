// The bridge's loopback transport on macOS; the contract is in transport.h.

#include "transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

uint64_t sevo_transport_now_ms(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000u;
}

// Waits for `events` on `fd` until `deadline_ms`: 1 when ready, 0 with errno set when
// the deadline passed or poll failed.
static int wait_for(int fd, short events, uint64_t deadline_ms) {
    for (;;) {
        uint64_t now = sevo_transport_now_ms();
        if (now >= deadline_ms) {
            errno = ETIMEDOUT;
            return 0;
        }
        uint64_t left = deadline_ms - now;
        struct pollfd p = { .fd = fd, .events = events, .revents = 0 };
        int ready = poll(&p, 1, left > INT_MAX ? INT_MAX : (int)left);
        if (ready > 0) return 1;
        if (ready < 0 && errno != EINTR) return 0;
    }
}

int sevo_transport_connect(int port, uint64_t deadline_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) == 0) return fd;
    if (errno == EINPROGRESS || errno == EINTR) {
        int error = 0;
        socklen_t size = sizeof error;
        if (wait_for(fd, POLLOUT, deadline_ms) && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0) {
            if (error == 0) return fd;
            errno = error;
        }
    }
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

int sevo_transport_send_all(int fd, const void *data, size_t n, uint64_t deadline_ms) {
    const uint8_t *p = (const uint8_t *)data;
    while (n) {
        ssize_t sent = send(fd, p, n, SEND_FLAGS);
        if (sent > 0) {
            p += sent;
            n -= (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_for(fd, POLLOUT, deadline_ms)) return 0;
            continue;
        }
        if (sent == 0) errno = ECONNRESET;
        return 0;
    }
    return 1;
}

int sevo_transport_recv_all(int fd, void *data, size_t n, uint64_t deadline_ms) {
    uint8_t *p = (uint8_t *)data;
    while (n) {
        ssize_t got = recv(fd, p, n, 0);
        if (got > 0) {
            p += got;
            n -= (size_t)got;
            continue;
        }
        if (got == 0) {
            errno = ECONNRESET;
            return 0;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!wait_for(fd, POLLIN, deadline_ms)) return 0;
            continue;
        }
        return 0;
    }
    return 1;
}
