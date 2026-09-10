/*
 * sevo-discord-bridge.exe — Discord's IPC socket, served inside the bottle.
 *
 * A Windows game reaches Discord through the named pipe \\.\pipe\discord-ipc-N.
 * The Discord client on macOS listens on a unix socket, $TMPDIR/discord-ipc-N.
 * This program serves the pipe and relays every byte to the socket and back,
 * so discord-rpc, the Game SDK and the Social SDK all connect unchanged.
 *
 * The framing is opaque here: the bridge parses nothing, which is what keeps a
 * game's own artwork, state and buttons intact.
 *
 * Build: make            (produces sevo-discord-bridge.exe)
 *
 * Usage: sevo-discord-bridge.exe [--dir <macOS directory holding the sockets>]
 *
 * A PE process sees Wine's Windows environment, so pass --dir. Without it the
 * bridge falls back to TMPDIR, TMP, TEMP and then /tmp, the order Discord
 * itself resolves.
 *
 * The socket is reached with WinSock AF_UNIX (wine-staging's ws2_32-af_unix,
 * carried by this fork), addressed as \\?\unix<path with backslashes> so no
 * drive mapping is needed. The separators must be backslashes: connect() hands
 * the name to wine_get_unix_file_name(), a socket node will not open, and its
 * fallback to the parent directory splits the name on a backslash. Discord
 * leaves dead socket files behind and takes the next free index on restart, so
 * every index from 0 to 9 is tried by connecting; the presence of a file says
 * nothing about liveness.
 *
 * Half a minute in, the bridge makes itself a Wine system process, so it never
 * holds a prefix open on its own and comes down with the bottle.
 *
 * Diagnostics go to stderr, which lands in the wine log.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <afunix.h>
#include <winternl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIPE_NAME "\\\\.\\pipe\\discord-ipc-0"
#define MUTEX_NAME "Local\\SevoDiscordBridge"
#define SOCKET_INDEXES 10
#define RELAY_BUFFER 16384

/* Free pipe instances waiting for a client. The count caps how many programs
 * can connect in the same instant, not how many can be connected: each accept
 * hands the client to its own thread and opens a replacement at once. A game
 * that finds every instance taken gets ERROR_PIPE_BUSY, which discord-rpc and
 * the Game SDK answer with WaitNamedPipe. */
#define LISTENERS 4

/* NtSetInformationProcess class that detaches a process from the prefix's
 * lifetime, so the bridge never keeps a bottle open after the game exits. */
#define ProcessWineMakeProcessSystem 1000

/* A prefix whose only process is a system one comes straight back down, so the
 * bridge stays an ordinary process long enough for the game to appear beside
 * it. Started after a client boot, that game is already there. */
#define GRACE_MILLISECONDS 30000

/* ------------------------------------------------------------------- log */

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("sevo:discord ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ---------------------------------------------------- the Discord socket */

/** The socket directory as given, for the diagnostics. */
static char g_socket_dir[UNIX_PATH_MAX];
/** The same directory in the form connect() takes: separators as backslashes. */
static char g_socket_dir_windows[UNIX_PATH_MAX];

/** Fills the socket directory from --dir, else the environment. */
static void resolve_socket_dir(int argc, char **argv) {
    const char *dir = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) dir = argv[++i];
    }
    if (!dir) dir = getenv("TMPDIR");
    if (!dir) dir = getenv("TMP");
    if (!dir) dir = getenv("TEMP");
    if (!dir) dir = "/tmp";

    strncpy(g_socket_dir, dir, sizeof(g_socket_dir) - 1);
    g_socket_dir[sizeof(g_socket_dir) - 1] = '\0';
    size_t len = strlen(g_socket_dir);
    while (len > 1 && g_socket_dir[len - 1] == '/') g_socket_dir[--len] = '\0';

    strcpy(g_socket_dir_windows, g_socket_dir);
    for (char *p = g_socket_dir_windows; *p; p++) {
        if (*p == '/') *p = '\\';
    }
}

/**
 * Connects to the first live Discord socket.
 *
 * - Parameter index: receives the index that answered.
 * - Returns: a connected socket, or INVALID_SOCKET when Discord is absent.
 */
static SOCKET connect_to_discord(int *index) {
    for (int n = 0; n < SOCKET_INDEXES; n++) {
        SOCKADDR_UN addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        int written = snprintf(addr.sun_path, sizeof(addr.sun_path),
                               "\\\\?\\unix%s\\discord-ipc-%d", g_socket_dir_windows, n);
        if (written < 0 || (size_t)written >= sizeof(addr.sun_path)) continue;

        SOCKET s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) return INVALID_SOCKET;
        if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            *index = n;
            return s;
        }
        closesocket(s);
    }
    return INVALID_SOCKET;
}

/* ------------------------------------------------------------- the relay */

struct client {
    HANDLE pipe;
    SOCKET sock;
    HANDLE reader;  /* the thread blocked in ReadFile on the pipe */
    int index;
};

/** Reads the socket and writes the pipe until either end closes. */
static DWORD WINAPI socket_to_pipe(LPVOID param) {
    struct client *c = param;
    char buffer[RELAY_BUFFER];

    for (;;) {
        int got = recv(c->sock, buffer, sizeof(buffer), 0);
        if (got <= 0) break;
        for (int sent = 0; sent < got;) {
            DWORD wrote = 0;
            if (!WriteFile(c->pipe, buffer + sent, (DWORD)(got - sent), &wrote, NULL) || wrote == 0)
                goto done;
            sent += (int)wrote;
        }
    }
done:
    /* Unblock the pipe read so the owning thread can tear the client down.
     * The pipe stays connected: a disconnect drops whatever the game has not
     * read yet, and Discord's last frame before it hangs up is usually the
     * CLOSE that says why. */
    CancelSynchronousIo(c->reader);
    return 0;
}

/** Relays one pipe client until either end closes, then tears the client down. */
static DWORD WINAPI serve_client(LPVOID param) {
    struct client *c = param;

    c->sock = connect_to_discord(&c->index);
    if (c->sock == INVALID_SOCKET) {
        say("no Discord socket under %s — disconnecting the client", g_socket_dir);
        DisconnectNamedPipe(c->pipe);
        CloseHandle(c->pipe);
        free(c);
        return 0;
    }
    say("connected to discord-ipc-%d", c->index);

    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &c->reader, 0, FALSE, DUPLICATE_SAME_ACCESS);

    HANDLE back = CreateThread(NULL, 0, socket_to_pipe, c, 0, NULL);

    char buffer[RELAY_BUFFER];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(c->pipe, buffer, sizeof(buffer), &got, NULL) || got == 0) break;
        for (DWORD sent = 0; sent < got;) {
            int wrote = send(c->sock, buffer + sent, (int)(got - sent), 0);
            if (wrote <= 0) goto done;
            sent += (DWORD)wrote;
        }
    }
done:
    shutdown(c->sock, SD_BOTH);
    if (back) {
        WaitForSingleObject(back, INFINITE);
        CloseHandle(back);
    }
    say("client on discord-ipc-%d disconnected", c->index);
    closesocket(c->sock);
    CloseHandle(c->reader);
    /* Let the game drain what already arrived before the handle goes. */
    FlushFileBuffers(c->pipe);
    CloseHandle(c->pipe);
    free(c);
    return 0;
}

/* ------------------------------------------------------------------ main */

/** Detaches this process from the prefix's lifetime, once the grace window passes. */
static DWORD WINAPI become_a_system_process(LPVOID param) {
    Sleep(GRACE_MILLISECONDS);
    HANDLE event = NULL;
    NTSTATUS status = NtSetInformationProcess(GetCurrentProcess(),
                                              (PROCESSINFOCLASS)ProcessWineMakeProcessSystem,
                                              &event, sizeof(event));
    if (status != 0)
        say("staying a normal process: NtSetInformationProcess returned %08lx",
            (unsigned long)status);
    return 0;
}

/** Holds one free pipe instance, handing each client that takes it to its own thread. */
static DWORD WINAPI accept_clients(LPVOID param) {
    for (;;) {
        HANDLE pipe = CreateNamedPipeA(PIPE_NAME, PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES,
                                       RELAY_BUFFER, RELAY_BUFFER, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            say("CreateNamedPipe failed with %lu", GetLastError());
            return 1;
        }
        if (!ConnectNamedPipe(pipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(pipe);
            continue;
        }

        struct client *c = calloc(1, sizeof(*c));
        if (!c) {
            CloseHandle(pipe);
            continue;
        }
        c->pipe = pipe;
        HANDLE thread = CreateThread(NULL, 0, serve_client, c, 0, NULL);
        if (thread) {
            CloseHandle(thread);
        } else {
            CloseHandle(pipe);
            free(c);
        }
    }
}

int main(int argc, char **argv) {
    HANDLE mutex = CreateMutexA(NULL, TRUE, MUTEX_NAME);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    resolve_socket_dir(argc, argv);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        say("WinSock refused to start");
        return 1;
    }

    HANDLE grace = CreateThread(NULL, 0, become_a_system_process, NULL, 0, NULL);
    if (grace) CloseHandle(grace);
    say("serving %s from %s", PIPE_NAME, g_socket_dir);

    for (int i = 1; i < LISTENERS; i++) {
        HANDLE listener = CreateThread(NULL, 0, accept_clients, NULL, 0, NULL);
        if (listener) CloseHandle(listener);
    }
    return (int)accept_clients(NULL);
}
