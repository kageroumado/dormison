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

// A buffer the callee may write: present or null, `capacity` bytes, primed with what
// the game had in it.
struct InOut {
    bool present = false;
    uint32_t capacity = 0;
    std::vector<uint8_t> store;
    void *ptr() { return present ? store.data() : nullptr; }
};

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
    // A read-only buffer: one blob, null included.
    Blob get_in() {
        Blob b;
        uint32_t n;
        const uint8_t *p = r.blob(&n);
        if (!p) return b;
        b.store.assign(p, p + n);
        b.data = b.store.data();
        b.size = n;
        return b;
    }
    InOut get_inout() {
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
        io.store.assign(io.capacity, 0);
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
