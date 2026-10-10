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

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <pthread.h>
#include <unistd.h>

#include "proxy_prelude.h"
#include "transport.h"

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
// still be starting when the game first asks, so the first connect retries for a while;
// once one wait has run out, or the helper has refused or stalled a hello or published
// that it cannot serve, the bridge is off for the process and every call fails at once.
// A connection whose round trip fails or runs past its deadline is retired, and that
// thread's later calls fail at once.
struct Connection {
    int fd = -1;
    bool tried = false;
    ~Connection() {
        if (fd >= 0) close(fd);
    }
};

static thread_local Connection connection;
static std::atomic<bool> disabled{false};   // no connection will ever work

// How long the helper gets to come up and publish a port: wine's own start is most of it.
constexpr uint64_t kStartupWaitMs = 30000;
// One loopback connect; the helper's listen queue answers at once when it is there.
constexpr uint64_t kConnectTimeoutMs = 2000;
// The hello and its reply: the helper answers it before touching Steam.
constexpr uint64_t kGreetingTimeoutMs = 10000;
// One call's request and reply. Steam answers most calls in microseconds; a few block on
// the client (ConnectToGlobalUser while it signs in, synchronous Remote Storage writes),
// so the deadline is generous, and a call past it means the client is wedged.
constexpr uint64_t kCallTimeoutMs = 30000;
// The pause between attempts while the helper starts.
constexpr useconds_t kRetryPauseUs = 100 * 1000;

static void disable(const char *why) {
    if (!disabled.exchange(true)) log("Steam is out of reach for this process: %s", why);
}

enum class Trip { ok, closed, timed_out };

// Sends one frame and reads the reply into `reply` (status and payload, length stripped),
// all within `timeout_ms`.
static Trip round_trip(int fd, const std::vector<uint8_t> &frame, std::vector<uint8_t> &reply, uint64_t timeout_ms) {
    uint64_t deadline = sevo_transport_now_ms() + timeout_ms;
    auto failure = [] { return errno == ETIMEDOUT ? Trip::timed_out : Trip::closed; };
    if (!sevo_transport_send_all(fd, frame.data(), frame.size(), deadline)) return failure();
    uint32_t length;
    if (!sevo_transport_recv_all(fd, &length, 4, deadline)) return failure();
    if (length < 4 || length > kMaxBlob + 1024) return Trip::closed;
    reply.resize(length);
    if (!sevo_transport_recv_all(fd, reply.data(), length, deadline)) return failure();
    return Trip::ok;
}

// The port the helper published, 0 while the file is not there yet, or -1 when the
// helper wrote that it cannot serve.
static int port_from_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char text[32] = "";
    if (!fgets(text, sizeof text, f)) text[0] = 0;
    fclose(f);
    if (strncmp(text, kPortFileFailed, strlen(kPortFileFailed)) == 0) return -1;
    int port = atoi(text);
    return port > 0 && port < 65536 ? port : 0;
}

enum class Hello { accepted, refused, closed, timed_out };

static Hello hello(int fd) {
    const char *token = getenv("SEVO_STEAM_BRIDGE_TOKEN");
    Writer w;
    w.u32(0);
    w.u32(kMethodHello);
    w.u64(0);
    w.str(token ? token : "");
    w.u64(bridge_protocol_hash);
    w.u32(kHelloClient);
    w.close(0);
    std::vector<uint8_t> reply;
    switch (round_trip(fd, w.buf, reply, kGreetingTimeoutMs)) {
    case Trip::ok: break;
    case Trip::closed: return Hello::closed;
    case Trip::timed_out: return Hello::timed_out;
    }
    Reader r(reply.data(), reply.size());
    return r.u32() == kStatusOK ? Hello::accepted : Hello::refused;
}

static int current_fd() {
    Connection &c = connection;
    if (c.fd >= 0) return c.fd;
    if (c.tried) return -1;
    c.tried = true;
    if (disabled) return -1;
    const char *port_text = getenv("SEVO_STEAM_BRIDGE_PORT");
    const char *port_file = getenv("SEVO_STEAM_BRIDGE_PORT_FILE");
    int port = port_text ? atoi(port_text) : 0;
    if (port <= 0 && !(port_file && *port_file)) {
        static std::once_flag once;
        std::call_once(once, [] { log("neither SEVO_STEAM_BRIDGE_PORT nor SEVO_STEAM_BRIDGE_PORT_FILE is set: Steam stays out of reach"); });
        return -1;
    }
    // A connection the helper closes before answering the hello (its cap on connections
    // that have not said hello yet, or a helper still starting) is tried again.
    uint64_t startup_deadline = sevo_transport_now_ms() + kStartupWaitMs;
    for (; !disabled && sevo_transport_now_ms() < startup_deadline; usleep(kRetryPauseUs)) {
        if (port <= 0) {
            port = port_from_file(port_file);
            if (port < 0) {
                disable("the helper could not start (its log is steambridge-<appid>.log in the bottle)");
                return -1;
            }
            if (port == 0) continue;
        }
        uint64_t now = sevo_transport_now_ms();
        int fd = sevo_transport_connect(port, std::min(now + kConnectTimeoutMs, startup_deadline));
        if (fd < 0) continue;
        switch (hello(fd)) {
        case Hello::accepted:
            c.fd = fd;
            if (tracing()) log("connected to port %d on thread %p", port, (void *)pthread_self());
            return fd;
        case Hello::closed:
            close(fd);
            continue;
        case Hello::refused:
            close(fd);
            disable("the helper refused the hello (token or protocol mismatch)");
            return -1;
        case Hello::timed_out:
            close(fd);
            disable("the helper did not answer the hello within 10 s");
            return -1;
        }
    }
    if (!disabled) disable(port > 0 ? "no helper answered within 30 s" : "the helper published no port within 30 s");
    return -1;
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
    switch (round_trip(fd, w.buf, reply, kCallTimeoutMs)) {
    case Trip::ok: break;
    case Trip::closed:
        log("%s: the helper went away", method_name(method));
        drop_connection();
        return false;
    case Trip::timed_out:
        log("%s: no answer within %llu s: this thread's connection is retired", method_name(method),
            (unsigned long long)(kCallTimeoutMs / 1000));
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
    w.blob(tmp.empty() ? "" : (const void *)tmp.data(), (uint32_t)tmp.size());
}

void Call::put_inout(const void *p, uint32_t count, uint32_t msize, uint32_t wsize, const Run *runs, size_t n) {
    w.u8(p ? 1 : 0);
    if (!p) return;
    std::vector<uint8_t> tmp;
    elements_to_win(tmp, p, count, msize, wsize, runs, n);
    w.u32((uint32_t)tmp.size());
    w.blob(tmp.empty() ? "" : (const void *)tmp.data(), (uint32_t)tmp.size());
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

void note_refused(const char *method, const char *param) {
    std::lock_guard<std::mutex> guard(notes_lock);
    if (noted.insert(std::string(method) + "/" + param).second)
        log("%s: %s holds a string or pointer option, which the bridge cannot carry: the call fails", method, param);
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

uint32_t path_out_to_mac(char *buffer, uint32_t capacity) {
    if (!buffer || capacity == 0) return 0;
    buffer[capacity - 1] = 0;
    std::string converted = path_to_mac(buffer);
    size_t needed = converted.size() + 1;
    if (needed > capacity) {
        log("a path of %zu bytes does not fit the game's %u-byte buffer", needed, capacity);
        buffer[0] = 0;
        return needed > UINT32_MAX ? UINT32_MAX : (uint32_t)needed;
    }
    memcpy(buffer, converted.c_str(), needed);
    return (uint32_t)needed;
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
    if (thread_exit) {
        // A pooled thread may come back with new work: it gets a connection of its own again.
        drop_connection();
        connection.tried = false;
    }
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
