// A fake Steamworks SDK for the generator's fixtures (tests/gen/test_gen.py): each
// method is one marshaling rule the generator must get right. The class names are real
// SDK class names, since the generator recognizes interfaces by name; the version
// strings are the fixtures' own.
#pragma once

#include <stdint.h>

typedef int32_t int32;
typedef uint32_t uint32;
typedef uint16_t uint16;
typedef uint64_t uint64;

#if defined(__APPLE__) || defined(__linux__)
#pragma pack(push, 4)
#else
#pragma pack(push, 8)
#endif

struct CSteamID {
    uint64 m_steamid;
};

struct FixtureAddr {
    uint32 m_ip;
    uint16 m_port;
};

enum ESteamNetworkingConfigDataType {
    k_ESteamNetworkingConfig_Int32 = 1,
    k_ESteamNetworkingConfig_Int64 = 2,
    k_ESteamNetworkingConfig_Float = 3,
    k_ESteamNetworkingConfig_String = 4,
    k_ESteamNetworkingConfig_Ptr = 5,
    k_ESteamNetworkingConfigDataType__Force32Bit = 0x7fffffff
};

// The SDK's option record: a pointer in two union arms, the tag beside it.
struct SteamNetworkingConfigValue_t {
    int32 m_eValue;
    ESteamNetworkingConfigDataType m_eDataType;
    union {
        int32_t m_int32;
        int64_t m_int64;
        float m_float;
        const char *m_string;
        void *m_ptr;
    } m_val;
};

// A pointer two levels down a union arm, with no tag rule.
struct FixtureVariant_t {
    int32 m_eKind;
    union {
        int32 m_n;
        struct {
            const char *m_psz;
        } m_inner;
    } m_u;
};

// A record of a size MSVC returns through a hidden pointer and no register holds.
struct FixtureTotals_t {
    int32 m_nCount;
    int64_t m_nSum;
};

#pragma pack(pop)

#define STEAMNETWORKINGSOCKETS_INTERFACE_VERSION "SteamNetworkingSocketsFixture001"

class ISteamNetworkingSockets {
public:
    // Count before its array, sized by a PARAM_OVERRIDES rule; tagged records.
    virtual uint32 CreateListenSocketIP(const FixtureAddr &localAddress, int nOptions,
                                        const SteamNetworkingConfigValue_t *pOptions) = 0;
    // Count before its array, sized by the name rule.
    virtual bool SetThings(int nThings, const int32 *pThings) = 0;
    // A read-only pointer with no size rule beside an integer that could count it.
    virtual bool PutValues(int nCount, const int32 *pValue) = 0;
    // Records with a pointer in a union arm, by pointer and by value.
    virtual bool SetVariant(const FixtureVariant_t *pVariant) = 0;
    virtual bool SetVariantByValue(FixtureVariant_t variant) = 0;
    // A string, a writable single and an interface returned by version.
    virtual bool Describe(const char *pchName, int32 *pnLength) = 0;
};

#define STEAMUSERSTATS_INTERFACE_VERSION "SteamUserStatsFixture001"

class ISteamUserStats {
public:
    // Overloaded virtuals: MSVC puts each group's later declaration first.
    virtual bool GetStat(const char *pchName, int32 *pData) = 0;
    virtual bool GetStat(const char *pchName, float *pData) = 0;
    virtual bool SetStat(const char *pchName, int32 nData) = 0;
    virtual bool SetStat(const char *pchName, float fData) = 0;
    // Record returns: 8 bytes, 16 bytes, and one holding a pointer.
    virtual CSteamID GetOwner() = 0;
    virtual FixtureTotals_t GetTotals() = 0;
    virtual FixtureVariant_t GetVariant() = 0;
    virtual void *GetInterface(const char *pchVersion) = 0;
};

#define STEAMAPPS_INTERFACE_VERSION "SteamAppsFixture001"

class ISteamApps {
public:
    // Named like several elements with nothing to count them: generation refuses to run.
    virtual bool ReadHandles(const int32 *pHandles) = 0;
};
