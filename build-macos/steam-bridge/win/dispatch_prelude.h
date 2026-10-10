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
// the game's buffer holds), primed with what the game had in it.
struct InOut {
    bool present = false;
    uint32_t capacity = 0;
    std::vector<uint8_t> store;
    void *ptr() { return present ? store.data() : nullptr; }
};

// What a count argument may say at most: the bytes this process allocated for the
// buffer it sizes, or no limit for a null buffer.
constexpr uint64_t kNoLimit = UINT64_MAX;
inline uint64_t capacity_of(const InOut &io) { return io.present ? io.capacity : kNoLimit; }
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
    if (!count.present || count.capacity < sizeof(T)) return;
    T n;
    std::memcpy(&n, count.store.data(), sizeof n);
    limit(n, capacity, unit);
    std::memcpy(count.store.data(), &n, sizeof n);
}

// One request, decoded in full before anything is called. Each getter fails the reader
// on a payload that does not fit its rule; a handler checks done() before its call.
struct Req {
    Reader r;
    uint32_t method = 0;
    uint64_t obj = 0;        // the handle the request names (issue_handle)
    void *target = nullptr;  // the interface object that handle resolved to

    bool done() const { return r.done(); }

    // A struct by value: `size` raw bytes.
    Blob get_struct(uint32_t size) {
        Blob b = Blob::zeroed(size);
        r.take(b.data, size);
        return b;
    }
    // A read-only buffer: one blob, null included, holding whole `unit`-byte elements,
    // and exactly `exact` bytes when that is set.
    Blob get_in(uint32_t unit, uint32_t exact = 0) {
        Blob b;
        uint32_t n;
        const uint8_t *p = r.blob(&n);
        if (!p) return b;
        if (n % unit != 0 || (exact && n != exact)) {
            r.failed = true;
            return b;
        }
        b.store.assign(p, p + n);
        b.size = n;
        // An empty buffer is still a buffer: the callee gets a pointer and a count of 0.
        if (b.store.empty()) b.store.resize(1, 0);
        b.data = b.store.data();
        return b;
    }
    // A writable buffer: present flag, then the game's capacity in whole `unit`-byte
    // elements (exactly `exact` bytes when set), then a blob of that many bytes.
    InOut get_inout(uint32_t unit, uint32_t exact = 0) {
        InOut io;
        io.present = r.u8() != 0;
        if (!io.present) return io;
        io.capacity = r.u32();
        uint32_t n;
        const uint8_t *p = r.blob(&n);
        if (r.failed || io.capacity > kMaxBlob || io.capacity % unit != 0 ||
            (exact && io.capacity != exact) || !p || n != io.capacity) {
            r.failed = true;
            io.present = false;
            io.capacity = 0;
            return io;
        }
        io.store.assign(p, p + n);
        if (io.store.empty()) io.store.resize(1, 0);
        return io;
    }
};

struct Rep {
    Writer w;
    void put_inout(const InOut &io) {
        if (io.present) w.blob(io.store.data(), io.capacity);
    }
};

// A generated handler: false refuses the request (bad request, nothing called).
typedef bool (*Handler)(Req &, Rep &);

inline void *slot(void *obj, unsigned index) {
    return (*static_cast<void ***>(obj))[index];
}

// A name an interface object is issued under, and the interface version (an index into
// the generated interfaces) whose methods may be called on it.
struct InterfaceName {
    const char *version;
    uint16_t index;
};

// The opaque handle the game's side uses for `object`, registered under the interface
// `version` names; the same pair always gets the same handle. 0 for a null object or a
// version no generated interface serves.
uint64_t issue_handle(void *object, const char *version);

// ISteamUtils::GetAPICallResult: the macOS side sends the Windows size it expects.
bool api_call_result(Req &q, Rep &p, unsigned slot);

}  // namespace bridge

extern const bridge::Handler bridge_handlers[];
extern const size_t bridge_handler_count;
extern const char *const bridge_method_names[];
extern const bridge::InterfaceName bridge_interface_names[];
extern const size_t bridge_interface_name_count;
extern const uint16_t bridge_method_interfaces[];
extern const uint64_t bridge_protocol_hash;
extern const bridge::CallbackDef bridge_callbacks[];
extern const size_t bridge_callback_count;
