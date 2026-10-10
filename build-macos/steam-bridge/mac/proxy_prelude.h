// What a generated macOS proxy (generated/mac/<Class>_<Version>.cpp) needs from the
// bridge runtime (bridge.cpp). No Steamworks types here: each proxy TU includes its own
// SDK version's headers after this file.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "wire.h"

namespace bridge {

// One request to the helper in the bottle, on this thread's connection. Arguments go
// into `w`; send() does the round trip and leaves the reply in `r`.
struct Call {
    Writer w;
    Reader r;
    std::vector<uint8_t> reply;
    uint32_t method;

    Call(uint64_t handle, uint32_t method);
    bool send();

    // A struct by value: its bytes in the Windows layout.
    void put_struct(const void *mac, uint32_t msize, uint32_t wsize, const Run *runs, size_t n);
    void get_struct(void *mac, uint32_t msize, uint32_t wsize, const Run *runs, size_t n);
    // `count` elements the callee only reads: one blob, null pointer included.
    void put_in(const void *p, uint32_t count, uint32_t msize, uint32_t wsize, const Run *runs, size_t n);
    // `count` elements the callee may write: present flag, Windows capacity, current contents.
    void put_inout(const void *p, uint32_t count, uint32_t msize, uint32_t wsize, const Run *runs, size_t n);
    void get_inout(void *p, uint32_t count, uint32_t msize, uint32_t wsize, const Run *runs, size_t n);
    // A copy of a reply string that outlives this call (a per-thread ring).
    const char *keep_str(const char *s);
};

// A count argument as the game passed it, clamped to what a buffer can be.
template <typename T> inline uint32_t count(T n) {
    if (n <= 0) return 0;
    if ((uint64_t)n > (uint64_t)kMaxBlob) return kMaxBlob;
    return (uint32_t)n;
}

// The proxy for a Windows-side interface object, created once per (version, handle).
void *proxy_for(const char *version, uint64_t handle);

void note_local(const char *method);
void note_unsupported(const char *method);
// A call whose tagged record (gen_bridge.py TAGGED_RECORDS) names a refused arm.
void note_refused(const char *method, const char *param);

// Path conversion between the bottle's Windows view and macOS.
std::string path_to_win(const char *mac_path);
std::string path_to_mac(const char *win_path);
// Rewrites an out-buffer holding a Windows path as a macOS path, in place within
// `capacity` bytes; answers the macOS path's size including the NUL. A size above
// `capacity` means it did not fit, and the buffer is left empty.
uint32_t path_out_to_mac(char *buffer, uint32_t capacity);

// ISteamUtils::GetAPICallResult through the bridge, with callback struct conversion.
bool api_call_result(uint64_t handle, uint32_t method, uint64_t call, void *callback,
                     int32_t callback_size, int32_t expected, bool *failed);

struct InterfaceDef {
    const char *version;
    void *(*create)(uint64_t handle);
};

}  // namespace bridge

extern const bridge::InterfaceDef bridge_interfaces[];
extern const size_t bridge_interface_count;
extern const char *const bridge_method_names[];
extern const size_t bridge_method_count;
extern const uint64_t bridge_protocol_hash;
extern const bridge::CallbackDef bridge_callbacks[];
extern const size_t bridge_callback_count;
