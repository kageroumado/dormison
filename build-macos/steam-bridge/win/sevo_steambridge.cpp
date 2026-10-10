/*
 * sevo-steambridge.exe — the Windows half of the Steam bridge.
 *
 * A native macOS game's steamclient.dylib (the bridge's own, mac/bridge.cpp) forwards
 * every Steamworks interface call here over loopback. This process runs in the bottle,
 * loads the real steamclient64.dll of the signed-in Steam client, and makes the same
 * call on the same interface version. The generated handlers (generated/win/) do the
 * per-method work; this file loads the dll, listens, checks each connection's hello,
 * and serves the bridge's own methods (CreateInterface and the Steam_* exports).
 *
 * Build: make win   (x86_64-w64-mingw32-g++)
 *
 * Environment:
 *   SteamAppId                the game, so steamclient knows whose pipe this is
 *   SEVO_STEAM_BRIDGE_PORT    listening port (0 or unset: any free port)
 *   SEVO_STEAM_BRIDGE_TOKEN   what every connection's hello must carry
 *   SEVO_STEAM_BRIDGE_IDLE    seconds to wait for a first client, default 120
 *
 * The port it bound is written to %LOCALAPPDATA%\Sevoflurane\steambridge-<appid>.port
 * and a transcript to steambridge-<appid>.log beside it. The process exits when the
 * last client disconnects, or after the idle wait with no client at all.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dispatch_prelude.h"

namespace {

FILE *g_log;
CRITICAL_SECTION g_log_lock;

void logf_(const char *fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    EnterCriticalSection(&g_log_lock);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_log_lock);
}

bool g_tracing;

/* ------------------------------------------------------------- steamclient */

#pragma pack(push, 8)
struct WinCallbackMsg {
    int32_t user;
    int32_t callback;
    uint8_t *param;
    int32_t param_size;
};
#pragma pack(pop)

typedef void *(*fn_CreateInterface)(const char *, int *);
typedef uint8_t (*fn_BGetCallback)(int32_t, WinCallbackMsg *, int32_t *);
typedef uint8_t (*fn_FreeLastCallback)(int32_t);
typedef uint8_t (*fn_GetAPICallResult)(int32_t, uint64_t, void *, int32_t, int32_t, uint8_t *);
typedef void (*fn_ReleaseThreadLocalMemory)(int32_t);
typedef uint8_t (*fn_IsKnownInterface)(const char *);
typedef void (*fn_NotifyMissingInterface)(int32_t, const char *);

struct {
    HMODULE dll;
    fn_CreateInterface CreateInterface;
    fn_BGetCallback BGetCallback;
    fn_FreeLastCallback FreeLastCallback;
    fn_GetAPICallResult GetAPICallResult;
    fn_ReleaseThreadLocalMemory ReleaseThreadLocalMemory;
    fn_IsKnownInterface IsKnownInterface;
    fn_NotifyMissingInterface NotifyMissingInterface;
} g_steam;

/* The running client's steamclient64.dll, from the key its steam_api64.dll reads. */
std::string steamclient_path() {
    HKEY key;
    std::string path;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return path;
    char value[MAX_PATH * 2];
    DWORD size = sizeof value, type = 0;
    if (RegQueryValueExA(key, "SteamClientDll64", NULL, &type, (BYTE *)value, &size) == ERROR_SUCCESS && type == REG_SZ)
        path.assign(value, strnlen(value, size));
    RegCloseKey(key);
    return path;
}

bool load_steamclient() {
    std::string path = steamclient_path();
    if (path.empty()) {
        logf_("HKCU\\Software\\Valve\\Steam\\ActiveProcess\\SteamClientDll64 is not set: is Steam running?");
        return false;
    }
    g_steam.dll = LoadLibraryExA(path.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_steam.dll) {
        logf_("LoadLibrary(%s) failed: %lu", path.c_str(), GetLastError());
        return false;
    }
#define RESOLVE(name) g_steam.name = (fn_##name)GetProcAddress(g_steam.dll, (std::string("Steam_") + #name).c_str())
    g_steam.CreateInterface = (fn_CreateInterface)GetProcAddress(g_steam.dll, "CreateInterface");
    RESOLVE(BGetCallback);
    RESOLVE(FreeLastCallback);
    RESOLVE(GetAPICallResult);
    RESOLVE(ReleaseThreadLocalMemory);
    RESOLVE(IsKnownInterface);
    RESOLVE(NotifyMissingInterface);
#undef RESOLVE
    if (!g_steam.CreateInterface || !g_steam.BGetCallback || !g_steam.FreeLastCallback ||
        !g_steam.GetAPICallResult) {
        logf_("%s lacks CreateInterface or the Steam_* callback exports", path.c_str());
        return false;
    }
    logf_("loaded %s", path.c_str());
    return true;
}

/* ------------------------------------------------------------ the methods */

const char *method_name(uint32_t method) {
    if (method >= bridge::kFirstGeneratedMethod && method - bridge::kFirstGeneratedMethod < bridge_handler_count)
        return bridge_method_names[method - bridge::kFirstGeneratedMethod];
    return "?";
}

/* Bridge-own methods; answers whether the method was one of them. */
bool serve_special(bridge::Req &q, bridge::Rep &p) {
    switch (q.method) {
    case bridge::kMethodCreateInterface: {
        const char *name = q.r.str();
        int code = 0;
        void *iface = g_steam.CreateInterface(name, &code);
        logf_("CreateInterface(%s) -> %p (code %d)", name ? name : "(null)", iface, code);
        p.w.u64((uint64_t)(uintptr_t)iface);
        p.w.i32(code);
        return true;
    }
    case bridge::kMethodBGetCallback: {
        int32_t pipe = q.r.i32();
        WinCallbackMsg msg;
        memset(&msg, 0, sizeof msg);
        int32_t ignored = 0;
        uint8_t ret = g_steam.BGetCallback(pipe, &msg, &ignored);
        p.w.u8(ret ? 1 : 0);
        if (ret) {
            p.w.i32(msg.user);
            p.w.i32(msg.callback);
            p.w.blob(msg.param, msg.param ? (uint32_t)msg.param_size : 0);
            if (g_tracing) logf_("callback %d, %d bytes", msg.callback, msg.param_size);
        }
        return true;
    }
    case bridge::kMethodFreeLastCallback: {
        int32_t pipe = q.r.i32();
        p.w.u8(g_steam.FreeLastCallback(pipe) ? 1 : 0);
        return true;
    }
    case bridge::kMethodGetAPICallResult: {
        int32_t pipe = q.r.i32();
        uint64_t call = q.r.u64();
        uint32_t size = q.r.u32();
        int32_t expected = q.r.i32();
        if (size > bridge::kMaxBlob) { q.r.failed = true; return true; }
        std::vector<uint8_t> buffer(size, 0);
        uint8_t failed = 0;
        uint8_t ret = g_steam.GetAPICallResult(pipe, call, buffer.data(), (int32_t)size, expected, &failed);
        p.w.u8(ret ? 1 : 0);
        p.w.u8(failed ? 1 : 0);
        p.w.blob(buffer.data(), size);
        if (g_tracing) logf_("GetAPICallResult(%llu, %d, %u) -> %d failed %d", (unsigned long long)call, expected, size, ret, failed);
        return true;
    }
    case bridge::kMethodReleaseThreadLocalMemory: {
        int32_t exiting = q.r.u8();
        if (g_steam.ReleaseThreadLocalMemory) g_steam.ReleaseThreadLocalMemory(exiting);
        return true;
    }
    case bridge::kMethodIsKnownInterface: {
        const char *name = q.r.str();
        p.w.u8(g_steam.IsKnownInterface && name ? g_steam.IsKnownInterface(name) : 0);
        return true;
    }
    case bridge::kMethodNotifyMissingInterface: {
        int32_t pipe = q.r.i32();
        const char *name = q.r.str();
        if (g_steam.NotifyMissingInterface && name) g_steam.NotifyMissingInterface(pipe, name);
        return true;
    }
    }
    return false;
}

}  // namespace

void bridge::api_call_result(bridge::Req &q, bridge::Rep &p, unsigned slot) {
    void *obj = q.object();
    uint64_t call = q.r.u64();
    uint32_t size = q.r.u32();
    int32_t expected = q.r.i32();
    uint8_t has_failed = q.r.u8();
    if (size > bridge::kMaxBlob) { q.r.failed = true; return; }
    std::vector<uint8_t> buffer(size, 0);
    uint8_t failed = 0;
    typedef uint8_t (*Fn)(void *, uint64_t, void *, int32_t, int32_t, uint8_t *);
    uint8_t ret = ((Fn)bridge::slot(obj, slot))(obj, call, buffer.data(), (int32_t)size, expected, has_failed ? &failed : NULL);
    p.w.u8(ret ? 1 : 0);
    p.w.u8(failed ? 1 : 0);
    p.w.blob(buffer.data(), size);
}

namespace {

/* --------------------------------------------------------------- transport */

std::string g_token;
volatile LONG g_clients;
volatile LONG g_ever_connected;

bool send_all(SOCKET s, const void *data, size_t n) {
    const char *p = static_cast<const char *>(data);
    while (n) {
        int w = send(s, p, (int)n, 0);
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

bool recv_all(SOCKET s, void *data, size_t n) {
    char *p = static_cast<char *>(data);
    while (n) {
        int r = recv(s, p, (int)n, 0);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

bool reply(SOCKET s, bridge::Rep &p, uint32_t status) {
    bridge::Writer frame;
    frame.u32(0);
    frame.u32(status);
    frame.bytes(p.w.buf.data(), p.w.buf.size());
    frame.close(0);
    return send_all(s, frame.buf.data(), frame.buf.size());
}

/* One request: returns false when the connection should end. */
bool serve_frame(SOCKET s, std::vector<uint8_t> &frame, bool &greeted) {
    bridge::Req q;
    q.r = bridge::Reader(frame.data(), frame.size());
    q.method = q.r.u32();
    q.obj = q.r.u64();
    bridge::Rep p;

    if (!greeted) {
        if (q.method != bridge::kMethodHello) return false;
        const char *token = q.r.str();
        uint64_t hash = q.r.u64();
        uint32_t version = q.r.u32();
        bool ok = token && token == g_token && hash == bridge_protocol_hash && version == 1;
        logf_("hello: protocol %016llx%s, version %u -> %s", (unsigned long long)hash,
              hash == bridge_protocol_hash ? "" : " (ours differs)", version, ok ? "accepted" : "refused");
        reply(s, p, ok ? bridge::kStatusOK : bridge::kStatusRefused);
        greeted = ok;
        return ok;
    }

    uint32_t status = bridge::kStatusOK;
    if (q.method < bridge::kFirstGeneratedMethod) {
        if (!serve_special(q, p)) status = bridge::kStatusUnknownMethod;
    } else {
        uint32_t index = q.method - bridge::kFirstGeneratedMethod;
        if (index >= bridge_handler_count || !bridge_handlers[index]) {
            logf_("no handler for method %u (%s)", q.method, method_name(q.method));
            status = bridge::kStatusUnknownMethod;
        } else {
            if (g_tracing) logf_("%s obj %p", method_name(q.method), q.object());
            bridge_handlers[index](q, p);
        }
    }
    if (q.r.failed) {
        logf_("%s: short request", method_name(q.method));
        status = bridge::kStatusBadRequest;
    }
    return reply(s, p, status);
}

DWORD WINAPI serve_client(LPVOID arg) {
    SOCKET s = (SOCKET)(uintptr_t)arg;
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    bool greeted = false;
    std::vector<uint8_t> frame;
    for (;;) {
        uint32_t length;
        if (!recv_all(s, &length, 4)) break;
        if (length < 12 || length > bridge::kMaxBlob + 1024) break;
        frame.resize(length);
        if (!recv_all(s, frame.data(), length)) break;
        if (!serve_frame(s, frame, greeted)) break;
    }
    if (g_steam.ReleaseThreadLocalMemory) g_steam.ReleaseThreadLocalMemory(1);
    closesocket(s);
    LONG left = InterlockedDecrement(&g_clients);
    logf_("client gone, %ld left", left);
    return 0;
}

std::string local_app_data() {
    char buffer[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buffer, sizeof buffer);
    if (n == 0 || n >= sizeof buffer) return "C:\\";
    return std::string(buffer) + "\\Sevoflurane";
}

}  // namespace

int main() {
    const char *appid = getenv("SteamAppId");
    const char *port_text = getenv("SEVO_STEAM_BRIDGE_PORT");
    const char *token = getenv("SEVO_STEAM_BRIDGE_TOKEN");
    const char *idle_text = getenv("SEVO_STEAM_BRIDGE_IDLE");
    const char *trace = getenv("SEVO_STEAM_BRIDGE_LOG");
    g_tracing = trace && *trace == '1';
    g_token = token ? token : "";
    int idle_seconds = idle_text ? atoi(idle_text) : 120;

    InitializeCriticalSection(&g_log_lock);
    std::string dir = local_app_data();
    CreateDirectoryA(dir.c_str(), NULL);
    std::string stem = dir + "\\steambridge-" + (appid ? appid : "0");
    g_log = fopen((stem + ".log").c_str(), "w");
    logf_("sevo-steambridge starting, app %s, protocol %016llx", appid ? appid : "(unset)",
          (unsigned long long)bridge_protocol_hash);
    if (!appid) logf_("SteamAppId is not set: steamclient will not know the game");
    if (g_token.empty()) {
        logf_("SEVO_STEAM_BRIDGE_TOKEN is not set: nothing could tell the game's connections from anyone else's, so not listening");
        return 1;
    }

    if (!load_steamclient()) return 1;

    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        logf_("WSAStartup failed");
        return 1;
    }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((u_short)(port_text ? atoi(port_text) : 0));
    if (bind(listener, (sockaddr *)&addr, sizeof addr) != 0 || listen(listener, 16) != 0) {
        logf_("bind/listen on port %s failed: %d", port_text ? port_text : "0", WSAGetLastError());
        return 1;
    }
    int len = sizeof addr;
    getsockname(listener, (sockaddr *)&addr, &len);
    int port = ntohs(addr.sin_port);
    {
        FILE *f = fopen((stem + ".port").c_str(), "w");
        if (f) {
            fprintf(f, "%d\n", port);
            fclose(f);
        }
    }
    logf_("listening on 127.0.0.1:%d", port);

    for (;;) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        timeval tv = {1, 0};
        int ready = select(0, &set, NULL, NULL, &tv);
        if (ready > 0) {
            SOCKET client = accept(listener, NULL, NULL);
            if (client == INVALID_SOCKET) continue;
            InterlockedIncrement(&g_clients);
            InterlockedExchange(&g_ever_connected, 1);
            HANDLE thread = CreateThread(NULL, 0, serve_client, (LPVOID)(uintptr_t)client, 0, NULL);
            if (thread) CloseHandle(thread);
            else { closesocket(client); InterlockedDecrement(&g_clients); }
            continue;
        }
        if (g_ever_connected) {
            if (g_clients == 0) {
                /* A brief grace: a game's threads reconnect one by one at start. */
                Sleep(2000);
                if (g_clients == 0) {
                    logf_("last client gone: exiting");
                    break;
                }
            }
        } else if (--idle_seconds <= 0) {
            logf_("no client within the idle wait: exiting");
            break;
        }
    }
    DeleteFileA((stem + ".port").c_str());
    closesocket(listener);
    WSACleanup();
    return 0;
}
