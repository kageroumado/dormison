// The wire format both halves of the Steam bridge speak: the macOS steamclient.dylib
// (clang, arm64 + x86_64) and sevo-steambridge.exe (mingw, x86_64). Plain C++17 with no
// Steamworks or platform headers, so the generated code on either side includes it.
//
// Every frame is little-endian. A request is
//     u32 length (of everything after this field)  u32 method  u64 object  payload
// and a reply is
//     u32 length  u32 status  payload
// The payload layout of each method is fixed by the generator, identically on both
// sides, from the rules in gen/gen_bridge.py. Structs travel in their Windows layout;
// the macOS side converts with the run tables the generator computes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace bridge {

// Method ids below this are the bridge's own (hello, CreateInterface, the Steam_*
// exports); generated interface methods start here.
constexpr uint32_t kFirstGeneratedMethod = 64;

enum SpecialMethod : uint32_t {
    kMethodHello = 1,               // token + protocol hash; the first message of a connection
    kMethodCreateInterface = 2,     // str name → u64 handle, i32 return code
    kMethodBGetCallback = 3,        // i32 pipe → u8 ret [, i32 user, i32 callback, blob params]
    kMethodFreeLastCallback = 4,    // i32 pipe → u8 ret
    kMethodGetAPICallResult = 5,    // i32 pipe, u64 call, u32 win size, i32 expected → u8 ret, u8 failed, blob
    kMethodReleaseThreadLocalMemory = 6,
    kMethodIsKnownInterface = 7,    // str → u8
    kMethodNotifyMissingInterface = 8,
};

enum Status : uint32_t {
    kStatusOK = 0,
    kStatusUnknownMethod = 1,
    kStatusBadRequest = 2,
    kStatusRefused = 3,             // hello rejected: token or protocol mismatch
};

// The hello's last field: what the connection is for. A client connection carries the
// protocol hash and serves calls; a keepalive (libsevosteamipc.dylib's, held from the
// game's start) carries only the token and serves nothing.
enum HelloKind : uint32_t {
    kHelloClient = 1,
    kHelloKeepalive = 2,
};

// The largest first frame the helper reads from a connection that has not said hello.
constexpr uint32_t kMaxHelloFrame = 4096;

// What the helper writes to its port file instead of a port when it cannot serve.
constexpr const char *kPortFileFailed = "failed";

// A blob is u32 length + bytes; this length means "null pointer".
constexpr uint32_t kNullBlob = 0xFFFFFFFFu;

// The largest buffer either side allocates on the other's say-so.
constexpr uint32_t kMaxBlob = 64u << 20;

struct Writer {
    std::vector<uint8_t> buf;

    void bytes(const void *p, size_t n) {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        buf.insert(buf.end(), b, b + n);
    }
    template <typename T> void raw(T v) { bytes(&v, sizeof v); }
    void u8(uint8_t v) { raw(v); }
    void u16(uint16_t v) { raw(v); }
    void u32(uint32_t v) { raw(v); }
    void u64(uint64_t v) { raw(v); }
    void i8(int8_t v) { raw(v); }
    void i16(int16_t v) { raw(v); }
    void i32(int32_t v) { raw(v); }
    void i64(int64_t v) { raw(v); }
    void f32(float v) { raw(v); }
    void f64(double v) { raw(v); }
    void blob(const void *p, uint32_t n) {
        if (!p) { u32(kNullBlob); return; }
        u32(n);
        bytes(p, n);
    }
    // NUL-terminated string, the NUL included; a null pointer travels as kNullBlob.
    void str(const char *s) {
        if (!s) { u32(kNullBlob); return; }
        blob(s, (uint32_t)std::strlen(s) + 1);
    }
    // Patches the u32 at `at` with the number of bytes written after it.
    void close(size_t at) {
        uint32_t n = (uint32_t)(buf.size() - at - 4);
        std::memcpy(buf.data() + at, &n, 4);
    }
};

struct Reader {
    const uint8_t *p = nullptr;
    const uint8_t *end = nullptr;
    bool failed = false;

    Reader() = default;
    Reader(const void *data, size_t n) : p(static_cast<const uint8_t *>(data)), end(p + n) {}

    size_t remaining() const { return failed ? 0 : (size_t)(end - p); }
    bool take(void *out, size_t n) {
        if (failed || (size_t)(end - p) < n) {
            failed = true;
            std::memset(out, 0, n);
            return false;
        }
        std::memcpy(out, p, n);
        p += n;
        return true;
    }
    template <typename T> T raw() { T v; take(&v, sizeof v); return v; }
    uint8_t u8() { return raw<uint8_t>(); }
    uint16_t u16() { return raw<uint16_t>(); }
    uint32_t u32() { return raw<uint32_t>(); }
    uint64_t u64() { return raw<uint64_t>(); }
    int8_t i8() { return raw<int8_t>(); }
    int16_t i16() { return raw<int16_t>(); }
    int32_t i32() { return raw<int32_t>(); }
    int64_t i64() { return raw<int64_t>(); }
    float f32() { return raw<float>(); }
    double f64() { return raw<double>(); }
    // A blob's bytes, pointing into the frame; nullptr for a null blob or a short frame,
    // with the length in *n.
    const uint8_t *blob(uint32_t *n) {
        uint32_t len = u32();
        *n = 0;
        if (failed || len == kNullBlob) return nullptr;
        if ((size_t)(end - p) < len) { failed = true; return nullptr; }
        const uint8_t *at = p;
        p += len;
        *n = len;
        return at;
    }
    // A string blob as a C string: nullptr for null, "" for a malformed one.
    const char *str() {
        uint32_t n;
        const uint8_t *b = blob(&n);
        if (!b) return nullptr;
        if (n == 0 || b[n - 1] != 0) return "";
        return reinterpret_cast<const char *>(b);
    }
};

// One copy in a struct conversion: `len` bytes at `win` on the Windows side are the
// bytes at `mac` on the macOS side. A struct whose layouts match needs no runs.
struct Run {
    uint32_t win;
    uint32_t mac;
    uint32_t len;
};

inline void win_to_mac(void *mac, const void *win, const Run *runs, size_t count) {
    for (size_t i = 0; i < count; i++)
        std::memcpy(static_cast<uint8_t *>(mac) + runs[i].mac,
                    static_cast<const uint8_t *>(win) + runs[i].win, runs[i].len);
}

inline void mac_to_win(void *win, const void *mac, const Run *runs, size_t count) {
    for (size_t i = 0; i < count; i++)
        std::memcpy(static_cast<uint8_t *>(win) + runs[i].win,
                    static_cast<const uint8_t *>(mac) + runs[i].mac, runs[i].len);
}

// One Steamworks callback struct: the id its k_iCallback names and its size on each
// side; several entries share an id when the struct changed across SDK versions.
struct CallbackDef {
    int32_t id;
    uint32_t win_size;
    uint32_t mac_size;
    const Run *runs;
    uint32_t run_count;
};

}  // namespace bridge
