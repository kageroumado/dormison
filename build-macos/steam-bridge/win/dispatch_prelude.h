// What a generated Windows handler (generated/win/<Class>_<Version>.cpp) needs from
// sevo-steambridge.exe. The handlers never see Steamworks headers: they call vtable
// slots through function pointers typed with the MSVC x64 signature, on buffers the
// request carried in the Windows layout.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "wire.h"

namespace bridge {

// Bytes the request carried, owned here so the callee gets an aligned copy.
struct Blob {
    std::vector<uint8_t> store;
    void *data = nullptr;
    uint32_t size = 0;

    static Blob zeroed(uint32_t n) {
        Blob b;
        b.store.assign(n, 0);
        b.data = b.store.data();
        b.size = n;
        return b;
    }
    template <typename T> T as() const {
        T v;
        std::memset(&v, 0, sizeof v);
        if (data) std::memcpy(&v, data, size < sizeof v ? size : sizeof v);
        return v;
    }
};

// A buffer the callee may write: present or null, `capacity` bytes on the wire (what
// the game's buffer holds), primed with what the game had in it. `store` can be larger
// than `capacity` when the callee always fills a fixed size.
struct InOut {
    bool present = false;
    uint32_t capacity = 0;
    std::vector<uint8_t> store;
    void *ptr() { return present ? store.data() : nullptr; }
};

// What a count argument may say at most: the bytes this process allocated for the
// buffer it sizes, or no limit for a null buffer.
constexpr uint64_t kNoLimit = UINT64_MAX;
inline uint64_t capacity_of(const InOut &io) { return io.present ? io.store.size() : kNoLimit; }
inline uint64_t capacity_of(const Blob &b) { return b.data ? b.size : kNoLimit; }

// Clamps a count of `unit`-byte elements to `capacity` bytes; negative counts become 0.
template <typename T> inline void limit(T &n, uint64_t capacity, uint32_t unit) {
    if (n < 0) { n = 0; return; }
    if (capacity == kNoLimit) return;
    uint64_t most = capacity / unit;
    if ((uint64_t)n > most) n = (T)most;
}

// The same for a count the callee reads through a pointer (`*punCount`).
template <typename T> inline void limit_at(InOut &count, uint64_t capacity, uint32_t unit) {
    if (!count.present || count.store.size() < sizeof(T)) return;
    T n;
    std::memcpy(&n, count.store.data(), sizeof n);
    limit(n, capacity, unit);
    std::memcpy(count.store.data(), &n, sizeof n);
}

struct Req {
    Reader r;
    uint32_t method = 0;
    uint64_t obj = 0;

    void *object() const { return (void *)(uintptr_t)obj; }

    // A struct by value: `size` raw bytes.
    Blob get_struct(uint32_t size) {
        Blob b = Blob::zeroed(size);
        r.take(b.data, size);
        return b;
    }
    // A read-only buffer: one blob, null included, zero-padded to `least` bytes.
    Blob get_in(uint32_t least = 0) {
        Blob b;
        uint32_t n;
        const uint8_t *p = r.blob(&n);
        if (!p) return b;
        b.store.assign(p, p + n);
        if (b.store.size() < least) b.store.resize(least, 0);
        b.size = (uint32_t)b.store.size();
        // An empty buffer is still a buffer: the callee gets a pointer and a count of 0.
        if (b.store.empty()) b.store.resize(1, 0);
        b.data = b.store.data();
        return b;
    }
    // A writable buffer of the game's capacity, allocated at `least` bytes at minimum.
    InOut get_inout(uint32_t least = 0) {
        InOut io;
        io.present = r.u8() != 0;
        if (!io.present) return io;
        io.capacity = r.u32();
        if (io.capacity > kMaxBlob) {
            r.failed = true;
            io.present = false;
            return io;
        }
        uint32_t n;
        const uint8_t *p = r.blob(&n);
        io.store.assign(io.capacity > least ? io.capacity : least, 0);
        if (p) std::memcpy(io.store.data(), p, n < io.capacity ? n : io.capacity);
        return io;
    }
};

struct Rep {
    Writer w;
    void put_inout(const InOut &io) {
        if (io.present) w.blob(io.store.data(), io.capacity);
    }
};

typedef void (*Handler)(Req &, Rep &);

inline void *slot(void *obj, unsigned index) {
    return (*static_cast<void ***>(obj))[index];
}

// ISteamUtils::GetAPICallResult: the macOS side sends the Windows size it expects.
void api_call_result(Req &q, Rep &p, unsigned slot);

}  // namespace bridge

extern const bridge::Handler bridge_handlers[];
extern const size_t bridge_handler_count;
extern const char *const bridge_method_names[];
extern const uint64_t bridge_protocol_hash;
extern const bridge::CallbackDef bridge_callbacks[];
extern const size_t bridge_callback_count;
