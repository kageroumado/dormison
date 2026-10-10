// libsevosteamipc.dylib — answers a native game's "where is Steam?" from inside the game.
//
// A macOS game's libsteam_api.dylib finds Steam by asking the Mach service
// `com.valvesoftware.steam.ipctool` (Steam for Mac's ipcserver) for the running
// client's executable path and pid. It checks the pid with kill(pid, 0), cuts the path
// at its last '/', appends "steamclient.dylib" and dlopens that. The dock shim injects
// this library into a game it runs natively (DYLD_INSERT_LIBRARIES): bootstrap_look_up
// for that one name returns a port served by a thread in this process, which answers
// with the bridge directory (SEVO_STEAM_BRIDGE_DIR, holding the bridge's
// steamclient.dylib) and the game's own pid. Every other lookup goes to launchd as usual,
// and Steam for Mac's own ipcserver is never contacted.
//
// From the game's first moments it also holds the helper's keepalive connection, so
// sevo-steambridge.exe lives as long as the game however late the game initializes
// Steam, and it ends the game when Steam's process for it (the dock shim's waiter) goes.
//
// The protocol, read from libsteam_api.dylib (SDK 1.5x–1.6x):
//   every message: mach_msg_header_t, msgh_id = protocol version 0x68, a 32-bit command
//   at offset 0x18; the reply carries msgh_id 0x68 back. The client receives into a buffer
//   of exactly 0x28 (version) or 0x228 (path) bytes, and the kernel appends an 8-byte
//   trailer, so the replies are 0x20 and 0x220 bytes.
//   command 100001 (version query): reply word at 0x1c = server version.
//   command 14 (GetSteamPath): pid at 0x1c, NUL-terminated path from 0x20.
//   command 100000 (stop request): acknowledged and ignored.

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/event.h>
#include <mach/mach.h>
#include <pthread.h>
#include <servers/bootstrap.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define IPCTOOL_SERVICE "com.valvesoftware.steam.ipctool"
#define IPC_PROTOCOL_VERSION 0x68
#define IPC_COMMAND_GET_STEAM_PATH 14
#define IPC_COMMAND_STOP 100000
#define IPC_COMMAND_VERSION 100001
#define IPC_REPLY_SIZE 0x220
#define IPC_SHORT_REPLY_SIZE 0x20
#define IPC_PATH_OFFSET 0x20
#define IPC_PATH_CAPACITY (IPC_REPLY_SIZE - IPC_PATH_OFFSET)

typedef struct {
    mach_msg_header_t header;
    uint32_t command;
    uint32_t argument;
    uint8_t padding[0x200];
    mach_msg_trailer_t trailer;
} ipc_request;

typedef struct {
    mach_msg_header_t header;
    uint32_t command;
    uint32_t value;
    char path[IPC_PATH_CAPACITY];
} ipc_reply;

static mach_port_t service_port = MACH_PORT_NULL;
static char steam_path[IPC_PATH_CAPACITY];

// SEVO_STEAM_IPC_LOG=1 traces lookups and requests on stderr.
static int tracing(void) {
    static int value = -1;
    if (value < 0) {
        const char *setting = getenv("SEVO_STEAM_IPC_LOG");
        value = setting && *setting == '1';
    }
    return value;
}

static void reply_to(const ipc_request *request) {
    if (tracing())
        fprintf(stderr, "sevo-steam-ipc: request id 0x%x command %u size %u\n",
                request->header.msgh_id, request->command, request->header.msgh_size);
    ipc_reply reply;
    memset(&reply, 0, sizeof(reply));
    reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(request->header.msgh_bits), 0);
    reply.header.msgh_remote_port = request->header.msgh_remote_port;
    reply.header.msgh_local_port = MACH_PORT_NULL;
    reply.header.msgh_id = IPC_PROTOCOL_VERSION;
    reply.command = request->command;
    switch (request->command) {
    case IPC_COMMAND_VERSION:
        reply.value = IPC_PROTOCOL_VERSION;
        reply.header.msgh_size = IPC_SHORT_REPLY_SIZE;
        break;
    case IPC_COMMAND_GET_STEAM_PATH:
        reply.value = (uint32_t)getpid();
        strlcpy(reply.path, steam_path, sizeof(reply.path));
        reply.header.msgh_size = IPC_REPLY_SIZE;
        break;
    default:
        reply.header.msgh_size = IPC_SHORT_REPLY_SIZE;
        break;
    }
    if (reply.header.msgh_remote_port == MACH_PORT_NULL) return;
    kern_return_t sent = mach_msg(&reply.header, MACH_SEND_MSG, reply.header.msgh_size, 0,
                                  MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (tracing()) fprintf(stderr, "sevo-steam-ipc: reply sent: 0x%x\n", sent);
}

static void *serve(void *unused) {
    (void)unused;
    pthread_setname_np("sevo-steam-ipc");
    for (;;) {
        ipc_request request;
        memset(&request, 0, sizeof(request));
        kern_return_t result = mach_msg(&request.header, MACH_RCV_MSG | MACH_RCV_LARGE, 0,
                                        sizeof(request), service_port, MACH_MSG_TIMEOUT_NONE,
                                        MACH_PORT_NULL);
        if (result == MACH_RCV_TOO_LARGE) continue;
        if (result != KERN_SUCCESS) return NULL;
        reply_to(&request);
    }
}

// Runs at the first lookup of the service: a game that never initializes Steamworks
// never gets the port or the thread.
static void start_service(void) {
    const char *directory = getenv("SEVO_STEAM_BRIDGE_DIR");
    if (!directory || !*directory) return;
    // libsteam_api keeps everything up to the last '/', so any file name works here.
    snprintf(steam_path, sizeof(steam_path), "%s/steam_osx", directory);
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &service_port) != KERN_SUCCESS) {
        service_port = MACH_PORT_NULL;
        return;
    }
    mach_port_insert_right(mach_task_self(), service_port, service_port, MACH_MSG_TYPE_MAKE_SEND);
    pthread_t thread;
    if (pthread_create(&thread, NULL, serve, NULL) != 0) {
        service_port = MACH_PORT_NULL;
        return;
    }
    pthread_detach(thread);
}

kern_return_t sevo_bootstrap_look_up(mach_port_t bp, const name_t name, mach_port_t *port) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    if (tracing()) fprintf(stderr, "sevo-steam-ipc: bootstrap_look_up(%s)\n", name ? name : "(null)");
    if (name && strcmp(name, IPCTOOL_SERVICE) == 0) {
        pthread_once(&once, start_service);
        if (service_port != MACH_PORT_NULL) {
            // A send right per lookup: the caller deallocates the one it was given.
            mach_port_mod_refs(mach_task_self(), service_port, MACH_PORT_RIGHT_SEND, 1);
            *port = service_port;
            return KERN_SUCCESS;
        }
    }
    return bootstrap_look_up(bp, name, port);
}

// The process that stands for this game in Steam (sevo-native.exe --wait, the
// dock shim's) is what Steam stops when the player stops the game, so the game
// ends with it.
static void *watch_waiter(void *argument) {
    pid_t waiter = (pid_t)(intptr_t)argument;
    pthread_setname_np("sevo-steam-waiter-watch");
    int queue = kqueue();
    if (queue < 0) return NULL;
    struct kevent watch;
    EV_SET(&watch, (uintptr_t)waiter, EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT, 0, NULL);
    // ESRCH: the waiter is already gone, so the game ends now.
    int registered = kevent(queue, &watch, 1, NULL, 0, NULL);
    int already_gone = registered < 0 && errno == ESRCH;
    if (registered < 0 && !already_gone) {
        close(queue);
        return NULL;
    }
    if (!already_gone) {
        struct kevent fired;
        if (kevent(queue, NULL, 0, &fired, 1, NULL) != 1) {
            close(queue);
            return NULL;
        }
    }
    close(queue);
    fprintf(stderr, "sevo-steam-ipc: Steam's process for this game (pid %d) is gone — ending\n", (int)waiter);
    kill(getpid(), SIGTERM);
    sleep(10);
    _exit(0);
}

// The keepalive: the helper exits when its last greeted connection closes, or when no
// game says hello within its idle wait, so this connection, opened as soon as the
// helper publishes its port and never used or closed, keeps it for the game's life.
// The hello matches shared/wire.h: u32 length, u32 method 1, u64 object 0, the token as
// a blob with its NUL, u64 protocol hash (0, unchecked for this kind), u32 kind 2.
#define KEEPALIVE_WAIT_SECONDS 120
#define HELLO_METHOD 1
#define HELLO_KIND_KEEPALIVE 2
#define PORT_FILE_FAILED "failed"

// The port in the helper's port file: 0 while absent, -1 when it says it cannot serve.
static int published_port(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char text[32] = "";
    if (!fgets(text, sizeof text, f)) text[0] = 0;
    fclose(f);
    if (strncmp(text, PORT_FILE_FAILED, strlen(PORT_FILE_FAILED)) == 0) return -1;
    int port = atoi(text);
    return port > 0 && port < 65536 ? port : 0;
}

static int send_keepalive_hello(int fd, const char *token) {
    uint32_t token_size = (uint32_t)strlen(token) + 1;
    uint8_t frame[4 + 4 + 8 + 4 + 256 + 8 + 4];
    if (token_size > 256) return 0;
    uint32_t length = 4 + 8 + 4 + token_size + 8 + 4, method = HELLO_METHOD, kind = HELLO_KIND_KEEPALIVE;
    uint64_t zero = 0;
    size_t at = 0;
    memcpy(frame + at, &length, 4); at += 4;
    memcpy(frame + at, &method, 4); at += 4;
    memcpy(frame + at, &zero, 8); at += 8;
    memcpy(frame + at, &token_size, 4); at += 4;
    memcpy(frame + at, token, token_size); at += token_size;
    memcpy(frame + at, &zero, 8); at += 8;
    memcpy(frame + at, &kind, 4); at += 4;
    if (write(fd, frame, at) != (ssize_t)at) return 0;
    uint8_t reply[8];
    size_t got = 0;
    while (got < sizeof reply) {
        ssize_t n = read(fd, reply + got, sizeof reply - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        got += (size_t)n;
    }
    uint32_t status;
    memcpy(&status, reply + 4, 4);
    return status == 0;
}

static void *hold_keepalive(void *unused) {
    (void)unused;
    pthread_setname_np("sevo-steam-keepalive");
    const char *port_text = getenv("SEVO_STEAM_BRIDGE_PORT");
    const char *port_file = getenv("SEVO_STEAM_BRIDGE_PORT_FILE");
    const char *token = getenv("SEVO_STEAM_BRIDGE_TOKEN");
    int fixed_port = port_text ? atoi(port_text) : 0;
    for (int attempt = 0; attempt < KEEPALIVE_WAIT_SECONDS * 4; attempt++, usleep(250 * 1000)) {
        int port = fixed_port > 0 ? fixed_port : port_file ? published_port(port_file) : 0;
        if (port < 0) return NULL;
        if (port == 0) continue;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return NULL;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        struct sockaddr_in address;
        memset(&address, 0, sizeof address);
        address.sin_family = AF_INET;
        address.sin_port = htons((uint16_t)port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
            close(fd);
            continue;
        }
        if (!send_keepalive_hello(fd, token)) {
            close(fd);
            fprintf(stderr, "sevo-steam-ipc: the helper refused the keepalive\n");
            return NULL;
        }
        if (tracing()) fprintf(stderr, "sevo-steam-ipc: keepalive held on port %d\n", port);
        return NULL;
    }
    return NULL;
}

// The write end of the dock shim's status pipe stays with the game alone: a process the
// game starts must not hold Steam's waiter open after the game is gone.
static void keep_status_pipe_private(void) {
    const char *text = getenv("SEVO_STEAM_BRIDGE_STATUS_FD");
    long fd = text ? strtol(text, NULL, 10) : -1;
    if (fd > 2) fcntl((int)fd, F_SETFD, FD_CLOEXEC);
    unsetenv("SEVO_STEAM_BRIDGE_STATUS_FD");
}

__attribute__((constructor)) static void sevo_steam_ipc_init(void) {
    keep_status_pipe_private();
    pthread_t thread;
    const char *token = getenv("SEVO_STEAM_BRIDGE_TOKEN");
    const char *port_file = getenv("SEVO_STEAM_BRIDGE_PORT_FILE");
    const char *port_text = getenv("SEVO_STEAM_BRIDGE_PORT");
    if (token && *token && ((port_file && *port_file) || (port_text && *port_text)) &&
        pthread_create(&thread, NULL, hold_keepalive, NULL) == 0)
        pthread_detach(thread);
    const char *text = getenv("SEVO_STEAM_BRIDGE_WAITER_PID");
    long pid = text ? strtol(text, NULL, 10) : 0;
    if (pid <= 0) return;
    if (pthread_create(&thread, NULL, watch_waiter, (void *)(intptr_t)pid) == 0) pthread_detach(thread);
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
    const void *replacement;
    const void *original;
} interposers[] = {
    { (const void *)sevo_bootstrap_look_up, (const void *)bootstrap_look_up },
};
