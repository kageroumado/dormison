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
// The protocol, read from libsteam_api.dylib (SDK 1.5x–1.6x):
//   every message: mach_msg_header_t, msgh_id = protocol version 0x68, a 32-bit command
//   at offset 0x18; the reply carries msgh_id 0x68 back. The client receives into a buffer
//   of exactly 0x28 (version) or 0x228 (path) bytes, and the kernel appends an 8-byte
//   trailer, so the replies are 0x20 and 0x220 bytes.
//   command 100001 (version query): reply word at 0x1c = server version.
//   command 14 (GetSteamPath): pid at 0x1c, NUL-terminated path from 0x20.
//   command 100000 (stop request): acknowledged and ignored.

#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <sys/event.h>
#include <mach/mach.h>
#include <pthread.h>
#include <servers/bootstrap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    if (kevent(queue, &watch, 1, NULL, 0, NULL) < 0 && errno != ESRCH) {
        close(queue);
        return NULL;
    }
    if (errno != ESRCH) {
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

__attribute__((constructor)) static void sevo_steam_ipc_init(void) {
    const char *text = getenv("SEVO_STEAM_BRIDGE_WAITER_PID");
    long pid = text ? strtol(text, NULL, 10) : 0;
    if (pid <= 0) return;
    pthread_t thread;
    if (pthread_create(&thread, NULL, watch_waiter, (void *)(intptr_t)pid) == 0) pthread_detach(thread);
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
    const void *replacement;
    const void *original;
} interposers[] = {
    { (const void *)sevo_bootstrap_look_up, (const void *)bootstrap_look_up },
};
