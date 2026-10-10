// The loopback transport both macOS halves of the Steam bridge share: steamclient.dylib's
// per-thread connections (bridge.cpp) and libsevosteamipc.dylib's keepalive
// (sevo_steam_ipc.c). Every socket it opens is non-blocking, close-on-exec, TCP_NODELAY
// and SO_NOSIGPIPE, and every wait on it runs against an absolute deadline on the
// monotonic clock: a helper that vanishes makes a call fail with EPIPE, and one that
// stalls makes it fail with ETIMEDOUT, so neither can end or hang the game.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEVO_TRANSPORT_API __attribute__((visibility("hidden")))

// Milliseconds on the monotonic clock, the unit of every deadline below.
SEVO_TRANSPORT_API uint64_t sevo_transport_now_ms(void);

// A connection to 127.0.0.1:`port`, or -1 with errno set once it is refused or
// `deadline_ms` passes.
SEVO_TRANSPORT_API int sevo_transport_connect(int port, uint64_t deadline_ms);

// Writes or reads exactly `n` bytes before `deadline_ms`; answers 1, or 0 with errno set:
// ETIMEDOUT when the deadline passed, ECONNRESET when the peer closed before the last
// byte, else the socket's own error. A short read or write only ever continues against
// the same deadline.
SEVO_TRANSPORT_API int sevo_transport_send_all(int fd, const void *data, size_t n, uint64_t deadline_ms);
SEVO_TRANSPORT_API int sevo_transport_recv_all(int fd, void *data, size_t n, uint64_t deadline_ms);

#ifdef __cplusplus
}
#endif
