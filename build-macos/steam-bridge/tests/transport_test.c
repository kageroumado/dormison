// The macOS transport (mac/transport.c) against fake helpers forked on loopback: one
// that never greets, one that vanishes mid-write, one that fragments its reply, one that
// trickles bytes past the deadline, and a port with nothing listening. Needs no Steam,
// wine or helper. SIGPIPE keeps its default action throughout, as in a game that never
// touched it: a write to a vanished peer that raised it would end this process.
//
// usage: transport_test        (exit status 0 when every case passes)

#include "transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

// How far past its deadline a timed-out wait may return: scheduling slack, no more.
#define SLACK_MS 150

static int failures;

static void check(int ok, const char *what) {
    printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) failures++;
}

// A loopback listener on a free port; the port lands in *port.
static int listen_any(int *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof address;
    if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof address) != 0 || listen(fd, 8) != 0 ||
        getsockname(fd, (struct sockaddr *)&address, &size) != 0) {
        perror("listener");
        exit(2);
    }
    *port = ntohs(address.sin_port);
    return fd;
}

typedef void (*peer_behavior)(int connection);

// Forks a fake helper that accepts one connection and runs `behavior` on it.
static pid_t fork_peer(int *port, peer_behavior behavior) {
    int listener = listen_any(port);
    pid_t pid = fork();
    if (pid == 0) {
        int connection = accept(listener, NULL, NULL);
        if (connection >= 0) behavior(connection);
        _exit(0);
    }
    close(listener);
    return pid;
}

static void reap(pid_t pid) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static void read_some(int connection, size_t n) {
    char buffer[256];
    while (n) {
        ssize_t got = read(connection, buffer, n < sizeof buffer ? n : sizeof buffer);
        if (got <= 0) return;
        n -= (size_t)got;
    }
}

// MARK: - Peers

static void never_greets(int connection) {
    read_some(connection, 16);
    pause();
}

static void vanishes_mid_write(int connection) {
    read_some(connection, 64);
    struct linger hard = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(connection, SOL_SOCKET, SO_LINGER, &hard, sizeof hard);
    close(connection);
}

static const char kReply[] = "a reply frame that arrives one byte at a time";

static void fragments_reply(int connection) {
    read_some(connection, 4);
    for (size_t i = 0; i < sizeof kReply; i++) {
        write(connection, kReply + i, 1);
        usleep(2000);
    }
    pause();
}

static void trickles_past_deadline(int connection) {
    read_some(connection, 4);
    for (int i = 0; i < 100; i++) {
        write(connection, "x", 1);
        usleep(100 * 1000);
    }
    pause();
}

// MARK: - Cases

static void case_never_greets(void) {
    int port;
    pid_t peer = fork_peer(&port, never_greets);
    int fd = sevo_transport_connect(port, sevo_transport_now_ms() + 2000);
    check(fd >= 0, "connect to a peer that accepts");
    uint8_t hello[16] = { 0 };
    uint64_t start = sevo_transport_now_ms();
    uint64_t deadline = start + 300;
    int sent = sevo_transport_send_all(fd, hello, sizeof hello, deadline);
    uint8_t reply[8];
    int got = sevo_transport_recv_all(fd, reply, sizeof reply, deadline);
    int error = errno;
    uint64_t elapsed = sevo_transport_now_ms() - start;
    check(sent && !got && error == ETIMEDOUT, "a hello nobody answers fails with ETIMEDOUT");
    check(elapsed >= 300 && elapsed < 300 + SLACK_MS, "  ...at its deadline");
    close(fd);
    reap(peer);
}

static void case_vanishes_mid_write(void) {
    int port;
    pid_t peer = fork_peer(&port, vanishes_mid_write);
    int fd = sevo_transport_connect(port, sevo_transport_now_ms() + 2000);
    size_t size = 16u << 20;
    uint8_t *big = calloc(1, size);
    uint64_t start = sevo_transport_now_ms();
    int sent = sevo_transport_send_all(fd, big, size, start + 5000);
    int error = errno;
    check(fd >= 0 && !sent && (error == EPIPE || error == ECONNRESET),
          "a write to a peer that vanishes fails (EPIPE/ECONNRESET), and SIGPIPE did not end the process");
    check(sevo_transport_now_ms() - start < 5000, "  ...before its deadline");
    sent = sevo_transport_send_all(fd, big, 64, sevo_transport_now_ms() + 500);
    check(!sent, "a second write to the closed connection fails too, still alive");
    free(big);
    close(fd);
    reap(peer);
}

static void case_fragments_reply(void) {
    int port;
    pid_t peer = fork_peer(&port, fragments_reply);
    int fd = sevo_transport_connect(port, sevo_transport_now_ms() + 2000);
    uint64_t deadline = sevo_transport_now_ms() + 3000;
    char reply[sizeof kReply];
    int ok = sevo_transport_send_all(fd, "ping", 4, deadline) &&
             sevo_transport_recv_all(fd, reply, sizeof reply, deadline);
    check(ok && memcmp(reply, kReply, sizeof kReply) == 0, "a reply in one-byte fragments arrives whole");
    close(fd);
    reap(peer);
}

static void case_trickles_past_deadline(void) {
    int port;
    pid_t peer = fork_peer(&port, trickles_past_deadline);
    int fd = sevo_transport_connect(port, sevo_transport_now_ms() + 2000);
    uint64_t start = sevo_transport_now_ms();
    uint64_t deadline = start + 350;
    char reply[64];
    int ok = sevo_transport_send_all(fd, "ping", 4, deadline) &&
             sevo_transport_recv_all(fd, reply, sizeof reply, deadline);
    int error = errno;
    uint64_t elapsed = sevo_transport_now_ms() - start;
    check(!ok && error == ETIMEDOUT, "a reply trickling a byte every 100 ms fails with ETIMEDOUT");
    check(elapsed >= 350 && elapsed < 350 + SLACK_MS, "  ...at the call's deadline: partial reads do not reset it");
    close(fd);
    reap(peer);
}

static void case_nothing_listening(void) {
    int port;
    close(listen_any(&port));
    uint64_t start = sevo_transport_now_ms();
    int fd = sevo_transport_connect(port, start + 2000);
    check(fd < 0 && errno == ECONNREFUSED, "a port with nothing listening is refused");
    check(sevo_transport_now_ms() - start < 100, "  ...at once");
}

int main(void) {
    signal(SIGPIPE, SIG_DFL);
    setvbuf(stdout, NULL, _IOLBF, 0);
    case_never_greets();
    case_vanishes_mid_write();
    case_fragments_reply();
    case_trickles_past_deadline();
    case_nothing_listening();
    printf("%s\n", failures ? "transport test FAILED" : "transport test passed");
    return failures ? 1 : 0;
}
