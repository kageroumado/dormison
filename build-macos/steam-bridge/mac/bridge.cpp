// steamclient.dylib — the macOS half of the Steam bridge.
//
// A native macOS game's libsteam_api.dylib dlopens this file where libsevosteamipc.dylib
// told it Steam lives, resolves CreateInterface and the Steam_* exports below, and from
// then on talks to Steamworks interfaces. Every interface it gets is a generated proxy
// (generated/mac/) whose methods marshal their arguments and send them over loopback to
// sevo-steambridge.exe in the bottle, which makes the same call on the real
// steamclient64.dll. This file is the runtime the proxies share: the per-thread
// connection, the proxy registry, the exports, callback conversion and path
// conversion.
//
// Environment, set by the dock shim when it execs the game natively:
//   SEVO_STEAM_BRIDGE_PORT    the helper's loopback port, or
//   SEVO_STEAM_BRIDGE_PORT_FILE  the file the helper writes its port to
//   SEVO_STEAM_BRIDGE_TOKEN   the per-launch token the helper expects first
//   SEVO_STEAM_BRIDGE_PREFIX  the bottle (WINEPREFIX), for drive-letter paths
//   SEVO_STEAM_BRIDGE_LOG     1 traces every call on stderr

#include <arpa/inet.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <set>
#include <sys/socket.h>
#include <unistd.h>

#include "proxy_prelude.h"

#define BRIDGE_EXPORT extern "C" __attribute__((visibility("default")))

namespace bridge {

static int tracing() {
    static int value = -1;
    if (value < 0) {
        const char *setting = getenv("SEVO_STEAM_BRIDGE_LOG");
        value = setting && *setting == '1';
    }
    return value;
}

static void log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("sevo-steambridge: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

static const char *method_name(uint32_t method) {
    if (method >= kFirstGeneratedMethod && method - kFirstGeneratedMethod < bridge_method_count)
        return bridge_method_names[method - kFirstGeneratedMethod];
    switch (method) {
    case kMethodHello: return "Hello";
    case kMethodCreateInterface: return "CreateInterface";
    case kMethodBGetCallback: return "Steam_BGetCallback";
    case kMethodFreeLastCallback: return "Steam_FreeLastCallback";
    case kMethodGetAPICallResult: return "Steam_GetAPICallResult";
    case kMethodReleaseThreadLocalMemory: return "Steam_ReleaseThreadLocalMemory";
    case kMethodIsKnownInterface: return "Steam_IsKnownInterface";
    case kMethodNotifyMissingInterface: return "Steam_NotifyMissingInterface";
    }
    return "?";
}

// MARK: - The connection

// One connection per game thread, opened at the thread's first call. The helper may
// still be starting when the game first asks, so the first connect retries for a while.
struct Connection {
    int fd = -1;
    bool tried = false;
    ~Connection() {
        if (fd >= 0) close(fd);
    }
};

static thread_local Connection connection;
static std::mutex disabled_lock;
static bool disabled = false;   // a refused hello: no connection will ever work

static bool write_all(int fd, const void *data, size_t n) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    while (n) {
        ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static bool read_all(int fd, void *data, size_t n) {
    uint8_t *p = static_cast<uint8_t *>(data);
    while (n) {
        ssize_t r = ::read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

// Sends one frame and reads the reply into `reply` (status and payload, length stripped).
static bool round_trip(int fd, const std::vector<uint8_t> &frame, std::vector<uint8_t> &reply) {
    if (!write_all(fd, frame.data(), frame.size())) return false;
    uint32_t length;
    if (!read_all(fd, &length, 4)) return false;
    if (length < 4 || length > kMaxBlob + 1024) return false;
    reply.resize(length);
    return read_all(fd, reply.data(), length);
}

// The port the helper published, or 0 while the file is not there yet.
static int port_from_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int port = 0;
    if (fscanf(f, "%d", &port) != 1) port = 0;
    fclose(f);
    return port > 0 && port < 65536 ? port : 0;
}

static bool hello(int fd);
static void hold_keepalive();

static int connect_once(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool hello(int fd) {
    const char *token = getenv("SEVO_STEAM_BRIDGE_TOKEN");
    Writer w;
    w.u32(0);
    w.u32(kMethodHello);
    w.u64(0);
    w.str(token ? token : "");
    w.u64(bridge_protocol_hash);
    w.u32(1);
    w.close(0);
    std::vector<uint8_t> reply;
    if (!round_trip(fd, w.buf, reply)) return false;
    Reader r(reply.data(), reply.size());
    uint32_t status = r.u32();
    if (status != kStatusOK) {
        log("the helper refused this connection (status %u): token or protocol mismatch", status);
        std::lock_guard<std::mutex> guard(disabled_lock);
        disabled = true;
        return false;
    }
    return true;
}

static int current_fd() {
    Connection &c = connection;
    if (c.fd >= 0) return c.fd;
    if (c.tried) return -1;
    c.tried = true;
    {
        std::lock_guard<std::mutex> guard(disabled_lock);
        if (disabled) return -1;
    }
    const char *port_text = getenv("SEVO_STEAM_BRIDGE_PORT");
    const char *port_file = getenv("SEVO_STEAM_BRIDGE_PORT_FILE");
    int port = port_text ? atoi(port_text) : 0;
    if (port <= 0 && !(port_file && *port_file)) {
        static std::once_flag once;
        std::call_once(once, [] { log("neither SEVO_STEAM_BRIDGE_PORT nor SEVO_STEAM_BRIDGE_PORT_FILE is set: Steam stays out of reach"); });
        return -1;
    }
    // Up to 30 s for the helper to come up: wine's own start is most of it.
    for (int attempt = 0; attempt < 300; attempt++) {
        if (port <= 0) {
            port = port_from_file(port_file);
            if (port <= 0) {
                usleep(100 * 1000);
                continue;
            }
        }
        int fd = connect_once(port);
        if (fd >= 0) {
            if (!hello(fd)) {
                close(fd);
                return -1;
            }
            c.fd = fd;
            if (tracing()) log("connected to port %d on thread %p", port, (void *)pthread_self());
            hold_keepalive();
            return fd;
        }
        {
            std::lock_guard<std::mutex> guard(disabled_lock);
            if (disabled) return -1;
        }
        usleep(100 * 1000);
    }
    log("no helper on port %d after 30 s", port);
    return -1;
}

// One connection that is never used and never closed: the helper exits when its last
// connection closes, and a game's threads come and go between calls, so this one keeps
// the helper alive for exactly as long as the process that holds it.
static void hold_keepalive() {
    static std::once_flag once;
    std::call_once(once, [] {
        const char *port_text = getenv("SEVO_STEAM_BRIDGE_PORT");
        const char *port_file = getenv("SEVO_STEAM_BRIDGE_PORT_FILE");
        int port = port_text ? atoi(port_text) : 0;
        if (port <= 0 && port_file) port = port_from_file(port_file);
        if (port <= 0) return;
        int fd = connect_once(port);
        if (fd < 0) return;
        if (!hello(fd)) close(fd);
    });
}

static void drop_connection() {
    Connection &c = connection;
    if (c.fd >= 0) close(c.fd);
    c.fd = -1;
}

// MARK: - Call

Call::Call(uint64_t handle, uint32_t method) : method(method) {
    w.u32(0);
    w.u32(method);
    w.u64(handle);
}

bool Call::send() {
    int fd = current_fd();
    if (fd < 0) return false;
    w.close(0);
    if (!round_trip(fd, w.buf, reply)) {
        log("%s: the helper went away", method_name(method));
        drop_connection();
        return false;
    }
    r = Reader(reply.data(), reply.size());
    uint32_t status = r.u32();
    if (tracing()) log("%s -> status %u, %zu bytes", method_name(method), status, reply.size() - 4);
    if (status != kStatusOK) {
        log("%s: status %u", method_name(method), status);
        return false;
    }
    return true;
}

void Call::put_struct(const void *mac, uint32_t msize, uint32_t wsize, const Run *runs, size_t n) {
    if (n == 0) {
        w.bytes(mac, wsize);
        return;
    }
    std::vector<uint8_t> tmp(wsize, 0);
    mac_to_win(tmp.data(), mac, runs, n);
    w.bytes(tmp.data(), wsize);
    (void)msize;
}

void Call::get_struct(void *mac, uint32_t msize, uint32_t wsize, const Run *runs, size_t n) {
    if (n == 0) {
        r.take(mac, wsize);
        return;
    }
    std::vector<uint8_t> tmp(wsize, 0);
    r.take(tmp.data(), wsize);
    win_to_mac(mac, tmp.data(), runs, n);
    (void)msize;
}

static void elements_to_win(std::vector<uint8_t> &out, const void *p, uint32_t count, uint32_t msize,
                            uint32_t wsize, const Run *runs, size_t n) {
    out.assign((size_t)count * wsize, 0);
    if (n == 0) {
        memcpy(out.data(), p, (size_t)count * wsize);
        return;
    }
    for (uint32_t i = 0; i < count; i++)
        mac_to_win(out.data() + (size_t)i * wsize, static_cast<const uint8_t *>(p) + (size_t)i * msize, runs, n);
}

void Call::put_in(const void *p, uint32_t count, uint32_t msize, uint32_t wsize, const Run *runs, size_t n) {
    if (!p) {
        w.u32(kNullBlob);
        return;
    }
    std::vector<uint8_t> tmp;
    elements_to_win(tmp, p, count, msize, wsize, runs, n);
    w.blob(tmp.data(), (uint32_t)tmp.size());
}

void Call::put_inout(const void *p, uint32_t count, uint32_t msize, uint32_t wsize, const Run *runs, size_t n) {
    w.u8(p ? 1 : 0);
    if (!p) return;
    std::vector<uint8_t> tmp;
    elements_to_win(tmp, p, count, msize, wsize, runs, n);
    w.u32((uint32_t)tmp.size());
    w.blob(tmp.data(), (uint32_t)tmp.size());
}

void Call::get_inout(void *p, uint32_t count, uint32_t msize, uint32_t wsize, const Run *runs, size_t n) {
    if (!p) return;
    uint32_t len;
    const uint8_t *data = r.blob(&len);
    if (!data) return;
    uint32_t have = len / wsize;
    if (have > count) have = count;
    if (n == 0) {
        memcpy(p, data, (size_t)have * wsize);
        return;
    }
    for (uint32_t i = 0; i < have; i++)
        win_to_mac(static_cast<uint8_t *>(p) + (size_t)i * msize, data + (size_t)i * wsize, runs, n);
}

// Strings the game may hold on to for a while: Steam's own contract is "valid until the
// next call", and a ring of recent strings per thread is more generous than that.
constexpr size_t kStringRing = 256;
static thread_local std::string string_ring[kStringRing];
static thread_local size_t string_next = 0;

const char *Call::keep_str(const char *s) {
    if (!s) return "";
    std::string &slot = string_ring[string_next++ % kStringRing];
    slot = s;
    return slot.c_str();
}

// MARK: - Proxies

static std::mutex proxies_lock;
static std::map<std::pair<std::string, uint64_t>, void *> proxies;

static const InterfaceDef *find_interface(const char *version) {
    for (size_t i = 0; i < bridge_interface_count; i++)
        if (strcmp(bridge_interfaces[i].version, version) == 0) return &bridge_interfaces[i];
    return nullptr;
}

void *proxy_for(const char *version, uint64_t handle) {
    if (!handle || !version) return nullptr;
    std::lock_guard<std::mutex> guard(proxies_lock);
    auto key = std::make_pair(std::string(version), handle);
    auto found = proxies.find(key);
    if (found != proxies.end()) return found->second;
    const InterfaceDef *def = find_interface(version);
    if (!def) {
        log("no proxy for interface %s", version);
        return nullptr;
    }
    void *proxy = def->create(handle);
    proxies[key] = proxy;
    if (tracing()) log("proxy %p for %s (handle %#llx)", proxy, version, (unsigned long long)handle);
    return proxy;
}

static std::mutex notes_lock;
static std::set<std::string> noted;

void note_local(const char *method) {
    std::lock_guard<std::mutex> guard(notes_lock);
    if (noted.insert(method).second) log("%s answered locally", method);
}

void note_unsupported(const char *method) {
    std::lock_guard<std::mutex> guard(notes_lock);
    if (noted.insert(method).second) log("%s is not bridged yet: answering zero", method);
}

// MARK: - Paths

static const char *prefix() {
    static const char *value = getenv("SEVO_STEAM_BRIDGE_PREFIX");
    return value ? value : "";
}

std::string path_to_mac(const char *win_path) {
    if (!win_path) return std::string();
    std::string s(win_path);
    if (s.size() >= 2 && isalpha((unsigned char)s[0]) && s[1] == ':') {
        std::string rest = s.substr(2);
        for (char &c : rest)
            if (c == '\\') c = '/';
        if (toupper((unsigned char)s[0]) == 'Z') return rest.empty() ? "/" : rest;
        std::string out = prefix();
        out += "/dosdevices/";
        out += (char)tolower((unsigned char)s[0]);
        out += ':';
        out += rest;
        return out;
    }
    return s;
}

std::string path_to_win(const char *mac_path) {
    if (!mac_path) return std::string();
    std::string s(mac_path);
    if (s.empty() || s[0] != '/') return s;
    std::string drive_c = std::string(prefix()) + "/drive_c";
    std::string out;
    if (!drive_c.empty() && s.compare(0, drive_c.size(), drive_c) == 0 &&
        (s.size() == drive_c.size() || s[drive_c.size()] == '/')) {
        out = "C:" + s.substr(drive_c.size());
        if (out.size() == 2) out += '/';
    } else {
        out = "Z:" + s;
    }
    for (char &c : out)
        if (c == '/') c = '\\';
    return out;
}

uint32_t path_out_to_mac(char *buffer, uint32_t capacity, uint32_t ret) {
    (void)ret;
    if (!buffer || capacity == 0) return 0;
    buffer[capacity - 1] = 0;
    std::string converted = path_to_mac(buffer);
    size_t n = converted.size();
    if (n >= capacity) n = capacity - 1;
    memcpy(buffer, converted.data(), n);
    buffer[n] = 0;
    return (uint32_t)n + 1;
}

// MARK: - Callbacks

static const CallbackDef *def_by_win_size(int32_t id, uint32_t win_size) {
    for (size_t i = 0; i < bridge_callback_count; i++)
        if (bridge_callbacks[i].id == id && bridge_callbacks[i].win_size == win_size) return &bridge_callbacks[i];
    return nullptr;
}

static const CallbackDef *def_by_mac_size(int32_t id, uint32_t mac_size) {
    for (size_t i = 0; i < bridge_callback_count; i++)
        if (bridge_callbacks[i].id == id && bridge_callbacks[i].mac_size == mac_size) return &bridge_callbacks[i];
    return nullptr;
}

// A callback payload in the Windows layout, converted into a buffer in the macOS
// layout; answers the macOS size.
static uint32_t convert_callback(int32_t id, const uint8_t *win, uint32_t win_size, std::vector<uint8_t> &out) {
    const CallbackDef *def = def_by_win_size(id, win_size);
    if (!def) {
        out.assign(win, win + win_size);
        return win_size;
    }
    out.assign(def->mac_size, 0);
    if (def->run_count == 0) memcpy(out.data(), win, win_size < def->mac_size ? win_size : def->mac_size);
    else win_to_mac(out.data(), win, def->runs, def->run_count);
    // SteamAPICallCompleted_t names the size of the result the game will fetch next.
    if (id == 703 && def->mac_size == 16 && win_size == 16) {
        int32_t inner_id;
        uint32_t inner_size;
        memcpy(&inner_id, out.data() + 8, 4);
        memcpy(&inner_size, out.data() + 12, 4);
        const CallbackDef *inner = def_by_win_size(inner_id, inner_size);
        if (inner) memcpy(out.data() + 12, &inner->mac_size, 4);
    }
    return def->mac_size;
}

#pragma pack(push, 4)
struct MacCallbackMsg {
    int32_t user;
    int32_t callback;
    uint8_t *param;
    int32_t param_size;
};
#pragma pack(pop)
static_assert(sizeof(MacCallbackMsg) == 20, "CallbackMsg_t on macOS is packed to 4");

static thread_local std::vector<uint8_t> last_callback;

bool api_call_result(uint64_t handle, uint32_t method, uint64_t call, void *callback, int32_t callback_size,
                     int32_t expected, bool *failed) {
    const CallbackDef *def = def_by_mac_size(expected, (uint32_t)callback_size);
    uint32_t win_size = def ? def->win_size : (uint32_t)count(callback_size);
    Call c(handle, method);
    c.w.u64(call);
    c.w.u32(win_size);
    c.w.i32(expected);
    c.w.u8(failed ? 1 : 0);
    if (!c.send()) return false;
    bool ret = c.r.u8() != 0;
    bool was_failed = c.r.u8() != 0;
    uint32_t len;
    const uint8_t *data = c.r.blob(&len);
    if (failed) *failed = was_failed;
    if (ret && data && callback) {
        std::vector<uint8_t> converted;
        uint32_t n = convert_callback(expected, data, len, converted);
        memcpy(callback, converted.data(), n < (uint32_t)callback_size ? n : (uint32_t)callback_size);
    }
    return ret;
}

}  // namespace bridge

// MARK: - Exports

using namespace bridge;

BRIDGE_EXPORT void *CreateInterface(const char *name, int *return_code) {
    Call c(0, kMethodCreateInterface);
    c.w.str(name);
    if (!c.send()) {
        if (return_code) *return_code = 1;
        return nullptr;
    }
    uint64_t handle = c.r.u64();
    int32_t code = c.r.i32();
    if (return_code) *return_code = code;
    void *proxy = proxy_for(name, handle);
    log("CreateInterface(%s) -> %p", name ? name : "(null)", proxy);
    return proxy;
}

BRIDGE_EXPORT bool Steam_BGetCallback(int32_t pipe, MacCallbackMsg *msg, int32_t *ignored) {
    (void)ignored;
    Call c(0, kMethodBGetCallback);
    c.w.i32(pipe);
    if (!c.send()) return false;
    if (!c.r.u8()) return false;
    int32_t user = c.r.i32();
    int32_t id = c.r.i32();
    uint32_t len;
    const uint8_t *data = c.r.blob(&len);
    if (!data || !msg) return false;
    uint32_t size = convert_callback(id, data, len, last_callback);
    msg->user = user;
    msg->callback = id;
    msg->param = last_callback.data();
    msg->param_size = (int32_t)size;
    if (tracing()) log("callback %d, %u -> %u bytes", id, len, size);
    return true;
}

BRIDGE_EXPORT bool Steam_FreeLastCallback(int32_t pipe) {
    Call c(0, kMethodFreeLastCallback);
    c.w.i32(pipe);
    if (!c.send()) return false;
    return c.r.u8() != 0;
}

BRIDGE_EXPORT bool Steam_GetAPICallResult(int32_t pipe, uint64_t call, void *callback, int32_t callback_size,
                                          int32_t expected, bool *failed) {
    const CallbackDef *def = def_by_mac_size(expected, (uint32_t)callback_size);
    uint32_t win_size = def ? def->win_size : count(callback_size);
    Call c(0, kMethodGetAPICallResult);
    c.w.i32(pipe);
    c.w.u64(call);
    c.w.u32(win_size);
    c.w.i32(expected);
    if (!c.send()) return false;
    bool ret = c.r.u8() != 0;
    bool was_failed = c.r.u8() != 0;
    uint32_t len;
    const uint8_t *data = c.r.blob(&len);
    if (failed) *failed = was_failed;
    if (ret && data && callback) {
        std::vector<uint8_t> converted;
        uint32_t n = convert_callback(expected, data, len, converted);
        memcpy(callback, converted.data(), n < (uint32_t)callback_size ? n : (uint32_t)callback_size);
    }
    return ret;
}

BRIDGE_EXPORT void Steam_ReleaseThreadLocalMemory(bool thread_exit) {
    Call c(0, kMethodReleaseThreadLocalMemory);
    c.w.u8(thread_exit ? 1 : 0);
    c.send();
    if (thread_exit) drop_connection();
}

BRIDGE_EXPORT bool Steam_IsKnownInterface(const char *version) {
    return version && find_interface(version) != nullptr;
}

BRIDGE_EXPORT void Steam_NotifyMissingInterface(int32_t pipe, const char *version) {
    (void)pipe;
    log("the game asked for %s, which this bridge does not serve", version ? version : "(null)");
}

BRIDGE_EXPORT void Breakpad_SteamMiniDumpInit(uint32_t, const char *, const char *) {}
BRIDGE_EXPORT void Breakpad_SteamSetAppID(uint32_t) {}
BRIDGE_EXPORT int Breakpad_SteamSetSteamID(uint64_t) { return 1; }
BRIDGE_EXPORT int Breakpad_SteamWriteMiniDumpSetComment(const char *) { return 1; }
BRIDGE_EXPORT void Breakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId(int, int) {}
