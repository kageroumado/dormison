// Drives the generated Windows handlers for the fixture SDK (tests/gen/sdk) against a
// fake interface object, compiled natively: a request either reaches the fake slot with
// counts that fit what arrived, or is refused with the slot never called.
//
// Built and run by test_gen.py together with the generated win/*.cpp of the fixtures.

#include <cstdio>
#include <cstring>
#include <vector>

#include "dispatch_prelude.h"

bool bridge_h_ISteamNetworkingSockets_SteamNetworkingSocketsFixture001_CreateListenSocketIP(bridge::Req &, bridge::Rep &);
bool bridge_h_ISteamNetworkingSockets_SteamNetworkingSocketsFixture001_SetThings(bridge::Req &, bridge::Rep &);
bool bridge_h_ISteamNetworkingSockets_SteamNetworkingSocketsFixture001_Describe(bridge::Req &, bridge::Rep &);

uint64_t bridge::issue_handle(void *object, const char *) { return object ? 1 : 0; }
bool bridge::api_call_result(bridge::Req &, bridge::Rep &, unsigned) { return false; }

namespace {

int failures;

void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        failures++;
    }
}

struct Seen {
    int calls = 0;
    int32_t count = -1;
    std::vector<uint8_t> bytes;
} seen;

uint32_t fake_create_listen(void *, const void *addr, int32_t n, const void *options) {
    seen.calls++;
    seen.count = n;
    seen.bytes.assign(static_cast<const uint8_t *>(options), static_cast<const uint8_t *>(options) + 16 * n);
    (void)addr;
    return 77;
}

uint8_t fake_set_things(void *, int32_t n, const void *things) {
    seen.calls++;
    seen.count = n;
    seen.bytes.assign(static_cast<const uint8_t *>(things), static_cast<const uint8_t *>(things) + 4 * n);
    return 1;
}

uint8_t fake_describe(void *, const char *name, void *length) {
    seen.calls++;
    int32_t n = (int32_t)std::strlen(name);
    std::memcpy(length, &n, 4);
    return 1;
}

void *vtable[6] = {(void *)&fake_create_listen, (void *)&fake_set_things, nullptr, nullptr, nullptr,
                   (void *)&fake_describe};
struct Fake {
    void **vtbl = vtable;
} fake;

typedef bool (*Handler)(bridge::Req &, bridge::Rep &);

// Runs one handler on `payload`; answers whether it accepted the request.
bool run(Handler h, const bridge::Writer &payload, bridge::Rep &rep) {
    seen = Seen();
    bridge::Req q;
    q.r = bridge::Reader(payload.buf.data(), payload.buf.size());
    q.target = &fake;
    return h(q, rep);
}

void option(bridge::Writer &w, int32_t value, int32_t type, int64_t payload) {
    w.i32(value);
    w.i32(type);
    w.i64(payload);
}

// CreateListenSocketIP(const FixtureAddr &, int nOptions, const SteamNetworkingConfigValue_t *)
bridge::Writer create_listen(int32_t n, int entries, int32_t type, uint32_t addr_size = 8) {
    bridge::Writer options;
    for (int i = 0; i < entries; i++) option(options, 10 + i, i == 1 ? type : 1, i);
    bridge::Writer w;
    std::vector<uint8_t> addr(addr_size, 0);
    w.blob(addr.data(), addr_size);
    w.i32(n);
    w.blob(options.buf.data(), (uint32_t)options.buf.size());
    return w;
}

void test_create_listen() {
    Handler h = &bridge_h_ISteamNetworkingSockets_SteamNetworkingSocketsFixture001_CreateListenSocketIP;
    bridge::Rep rep;

    check(run(h, create_listen(2, 2, 2), rep) && seen.calls == 1 && seen.count == 2,
          "two numeric options reach the slot with nOptions 2");
    int32_t second;
    std::memcpy(&second, seen.bytes.data() + 16, 4);
    check(seen.bytes.size() == 32 && second == 11, "both option records reach the slot");

    check(run(h, create_listen(5, 2, 3), rep) && seen.count == 2, "a count past the records is clamped to them");
    check(run(h, create_listen(-3, 2, 1), rep) && seen.count == 0, "a negative count becomes 0");

    check(!run(h, create_listen(2, 2, 4), rep) && seen.calls == 0, "a string option is refused before the slot");
    check(!run(h, create_listen(2, 2, 5), rep) && seen.calls == 0, "a pointer option is refused before the slot");
    check(!run(h, create_listen(2, 2, 0), rep) && seen.calls == 0, "an unknown tag is refused before the slot");

    bridge::Writer trailing = create_listen(2, 2, 1);
    trailing.u8(0);
    check(!run(h, trailing, rep) && seen.calls == 0, "a trailing byte is refused");

    bridge::Writer shorter = create_listen(2, 2, 1);
    shorter.buf.pop_back();
    check(!run(h, shorter, rep) && seen.calls == 0, "a short payload is refused");

    check(!run(h, create_listen(1, 1, 1, 6), rep) && seen.calls == 0, "a single record of the wrong size is refused");

    bridge::Writer ragged = create_listen(1, 1, 1);
    ragged.buf[ragged.buf.size() - 16 - 4] = 20;  // the options blob length: 20 bytes is no whole record
    ragged.buf.insert(ragged.buf.end(), 4, 0);
    check(!run(h, ragged, rep) && seen.calls == 0, "an options blob of no whole records is refused");
}

void test_set_things() {
    Handler h = &bridge_h_ISteamNetworkingSockets_SteamNetworkingSocketsFixture001_SetThings;
    bridge::Rep rep;
    int32_t things[3] = {4, 5, 6};
    bridge::Writer w;
    w.i32(3);
    w.blob(things, sizeof things);
    check(run(h, w, rep) && seen.count == 3 && std::memcmp(seen.bytes.data(), things, 12) == 0,
          "a count named before its array sizes it");

    bridge::Writer over;
    over.i32(9);
    over.blob(things, sizeof things);
    check(run(h, over, rep) && seen.count == 3, "a count before its array is clamped to the elements sent");
}

void test_describe() {
    Handler h = &bridge_h_ISteamNetworkingSockets_SteamNetworkingSocketsFixture001_Describe;
    int32_t length = 0;

    bridge::Rep rep;
    bridge::Writer w;
    w.str("lobby");
    w.u8(1);
    w.u32(4);
    w.blob(&length, 4);
    check(run(h, w, rep) && seen.calls == 1, "a well-formed string and out value reach the slot");
    bridge::Reader r(rep.w.buf.data(), rep.w.buf.size());
    uint8_t ret = r.u8();
    uint32_t n;
    const uint8_t *out = r.blob(&n);
    int32_t written = 0;
    if (out && n == 4) std::memcpy(&written, out, 4);
    check(ret == 1 && written == 5 && r.done(), "the reply carries the result and the written value");

    bridge::Rep rep2;
    bridge::Writer unterminated;
    unterminated.blob("lobby", 5);
    unterminated.u8(1);
    unterminated.u32(4);
    unterminated.blob(&length, 4);
    check(!run(h, unterminated, rep2) && seen.calls == 0, "a string without its NUL is refused");

    bridge::Rep rep3;
    bridge::Writer mismatched;
    mismatched.str("lobby");
    mismatched.u8(1);
    mismatched.u32(4);
    mismatched.blob(&length, 2);
    check(!run(h, mismatched, rep3) && seen.calls == 0, "a writable buffer whose contents differ from its capacity is refused");
}

void test_tags() {
    uint8_t records[32] = {};
    int32_t tag = 3;
    std::memcpy(records + 4, &tag, 4);
    tag = 2;
    std::memcpy(records + 20, &tag, 4);
    check(bridge::tags_allowed(records, 2, 16, 4, 0xe), "Int64 and Float tags pass");
    tag = 5;
    std::memcpy(records + 20, &tag, 4);
    check(!bridge::tags_allowed(records, 2, 16, 4, 0xe), "a Ptr tag fails");
    tag = -1;
    std::memcpy(records + 20, &tag, 4);
    check(!bridge::tags_allowed(records, 2, 16, 4, 0xe), "a negative tag fails");
    check(bridge::tags_allowed(nullptr, 9, 16, 4, 0xe), "a null array passes");
}

}  // namespace

int main() {
    test_create_listen();
    test_set_things();
    test_describe();
    test_tags();
    if (failures) return 1;
    std::printf("handlers: all checks passed\n");
    return 0;
}
