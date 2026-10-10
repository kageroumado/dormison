// A native Steamworks client for the bridge: loads a game's libsteam_api.dylib and
// drives it through the flat API, as Steamworks.NET does. Run it with the bridge
// environment the shim gives a game (SEVO_STEAM_BRIDGE_*, SteamAppId, and
// DYLD_INSERT_LIBRARIES=libsevosteamipc.dylib) from the game's own directory.
//
//   probe <path to libsteam_api.dylib> [achievement-to-unlock-then-clear]
//
// Checks, in order: SteamAPI_Init, SteamID and persona name, the app's install
// directory as a macOS path, a Remote Storage write/read/delete round trip, the stats
// callback (UserStatsReceived_t, 1101), the achievement list, and, when an achievement
// is named, SetAchievement + StoreStats with its UserAchievementStored_t (1103), then
// ClearAchievement + StoreStats so the account is left as it was.
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#pragma pack(push, 4)
typedef struct {
    int32_t user;
    int32_t callback;
    uint8_t *param;
    int32_t param_size;
} CallbackMsg;
#pragma pack(pop)

static void *lib;
static int failures;

static void *sym(const char *name) {
    void *p = dlsym(lib, name);
    if (!p) {
        printf("FAIL  missing export %s\n", name);
        failures++;
    }
    return p;
}

#define CHECK(cond, ...) do { if (cond) printf("ok    " __VA_ARGS__); else { printf("FAIL  " __VA_ARGS__); failures++; } printf("\n"); } while (0)

typedef bool (*f_bool)(void);
typedef void (*f_void)(void);
typedef int32_t (*f_i32)(void);
typedef void *(*f_create)(const char *);
typedef void *(*f_get_iface)(void *, int32_t, int32_t, const char *);

static f_void run_frame;
static bool (*next_callback)(int32_t, CallbackMsg *);
static void (*free_callback)(int32_t);
static int32_t pipe_handle;

// Pumps callbacks for up to `seconds`, answering whether `wanted` arrived.
static bool wait_callback(int wanted, int seconds, uint8_t *copy, int copy_size) {
    for (int i = 0; i < seconds * 20; i++) {
        run_frame();
        CallbackMsg msg;
        while (next_callback(pipe_handle, &msg)) {
            if (msg.callback == wanted) {
                if (copy) memcpy(copy, msg.param, msg.param_size < copy_size ? msg.param_size : copy_size);
                free_callback(pipe_handle);
                return true;
            }
            free_callback(pipe_handle);
        }
        usleep(50 * 1000);
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: probe <libsteam_api.dylib> [achievement]\n");
        return 2;
    }
    lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 2;
    }
    f_bool init = sym("SteamAPI_Init");
    f_void shutdown = sym("SteamAPI_Shutdown");
    f_void manual_init = sym("SteamAPI_ManualDispatch_Init");
    run_frame = sym("SteamAPI_ManualDispatch_RunFrame");
    next_callback = sym("SteamAPI_ManualDispatch_GetNextCallback");
    free_callback = sym("SteamAPI_ManualDispatch_FreeLastCallback");
    f_i32 get_user = sym("SteamAPI_GetHSteamUser");
    f_i32 get_pipe = sym("SteamAPI_GetHSteamPipe");
    f_create create = sym("SteamInternal_CreateInterface");
    f_get_iface get_stats = sym("SteamAPI_ISteamClient_GetISteamUserStats");
    f_get_iface get_storage = sym("SteamAPI_ISteamClient_GetISteamRemoteStorage");
    f_get_iface get_friends = sym("SteamAPI_ISteamClient_GetISteamFriends");
    f_get_iface get_apps = sym("SteamAPI_ISteamClient_GetISteamApps");
    f_get_iface get_user_iface = sym("SteamAPI_ISteamClient_GetISteamUser");
    uint64_t (*get_steam_id)(void *) = sym("SteamAPI_ISteamUser_GetSteamID");
    const char *(*persona)(void *) = sym("SteamAPI_ISteamFriends_GetPersonaName");
    uint32_t (*install_dir)(void *, uint32_t, char *, uint32_t) = sym("SteamAPI_ISteamApps_GetAppInstallDir");
    const char *(*language)(void *) = sym("SteamAPI_ISteamApps_GetCurrentGameLanguage");
    bool (*file_write)(void *, const char *, const void *, int32_t) = sym("SteamAPI_ISteamRemoteStorage_FileWrite");
    int32_t (*file_read)(void *, const char *, void *, int32_t) = sym("SteamAPI_ISteamRemoteStorage_FileRead");
    bool (*file_exists)(void *, const char *) = sym("SteamAPI_ISteamRemoteStorage_FileExists");
    bool (*file_persisted)(void *, const char *) = sym("SteamAPI_ISteamRemoteStorage_FilePersisted");
    bool (*file_delete)(void *, const char *) = sym("SteamAPI_ISteamRemoteStorage_FileDelete");
    int32_t (*file_count)(void *) = sym("SteamAPI_ISteamRemoteStorage_GetFileCount");
    const char *(*file_name_size)(void *, int, int32_t *) = sym("SteamAPI_ISteamRemoteStorage_GetFileNameAndSize");
    bool (*request_stats)(void *) = sym("SteamAPI_ISteamUserStats_RequestCurrentStats");
    uint32_t (*num_achievements)(void *) = sym("SteamAPI_ISteamUserStats_GetNumAchievements");
    const char *(*achievement_name)(void *, uint32_t) = sym("SteamAPI_ISteamUserStats_GetAchievementName");
    bool (*get_achievement)(void *, const char *, bool *) = sym("SteamAPI_ISteamUserStats_GetAchievement");
    bool (*set_achievement)(void *, const char *) = sym("SteamAPI_ISteamUserStats_SetAchievement");
    bool (*clear_achievement)(void *, const char *) = sym("SteamAPI_ISteamUserStats_ClearAchievement");
    bool (*store_stats)(void *) = sym("SteamAPI_ISteamUserStats_StoreStats");
    const char *(*display_attr)(void *, const char *, const char *) = sym("SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute");
    if (failures) return 1;

    CHECK(init(), "SteamAPI_Init");
    if (failures) return 1;
    manual_init();
    int32_t user = get_user();
    pipe_handle = get_pipe();
    CHECK(user > 0 && pipe_handle > 0, "user %d pipe %d", user, pipe_handle);
    void *client = create("SteamClient020");
    CHECK(client != NULL, "SteamClient020 %p", client);
    void *stats = get_stats(client, user, pipe_handle, "STEAMUSERSTATS_INTERFACE_VERSION012");
    void *storage = get_storage(client, user, pipe_handle, "STEAMREMOTESTORAGE_INTERFACE_VERSION016");
    void *friends = get_friends(client, user, pipe_handle, "SteamFriends017");
    void *apps = get_apps(client, user, pipe_handle, "STEAMAPPS_INTERFACE_VERSION008");
    void *steam_user = get_user_iface(client, user, pipe_handle, "SteamUser021");
    CHECK(stats && storage && friends && apps && steam_user, "interfaces stats %p storage %p friends %p apps %p user %p",
          stats, storage, friends, apps, steam_user);
    if (failures) return 1;

    uint64_t id = get_steam_id(steam_user);
    CHECK(id > 76561197960265728ull, "SteamID %llu", (unsigned long long)id);
    const char *name = persona(friends);
    CHECK(name && *name, "persona name '%s'", name ? name : "(null)");
    CHECK(language(apps) != NULL, "game language '%s'", language(apps));
    char dir[1024] = "";
    uint32_t n = install_dir(apps, (uint32_t)atoi(getenv("SteamAppId") ? getenv("SteamAppId") : "0"), dir, sizeof dir);
    CHECK(n > 0 && dir[0] == '/' && access(dir, F_OK) == 0, "install dir '%s' (%u)", dir, n);

    // Remote Storage round trip on a file of our own.
    const char *probe_file = "sevo-bridge-probe.txt";
    const char payload[] = "sevoflurane steam bridge probe\n";
    CHECK(file_write(storage, probe_file, payload, sizeof payload), "FileWrite %s", probe_file);
    CHECK(file_exists(storage, probe_file), "FileExists");
    char back[64] = "";
    int32_t got = file_read(storage, probe_file, back, sizeof back);
    CHECK(got == (int32_t)sizeof payload && memcmp(back, payload, sizeof payload) == 0, "FileRead %d bytes match", got);
    CHECK(file_persisted(storage, probe_file), "FilePersisted");
    int32_t count = file_count(storage);
    printf("info  %d files in Remote Storage:", count);
    for (int i = 0; i < count && i < 20; i++) {
        int32_t size = 0;
        const char *fname = file_name_size(storage, i, &size);
        printf(" %s(%d)", fname ? fname : "?", size);
    }
    printf("\n");
    CHECK(file_delete(storage, probe_file) && !file_exists(storage, probe_file), "FileDelete");

    // Stats: the request completes through a callback the bridge must convert.
    CHECK(request_stats(stats), "RequestCurrentStats");
    uint8_t received[24] = {0};
    CHECK(wait_callback(1101, 10, received, sizeof received), "UserStatsReceived_t arrived");
    uint64_t game_id;
    memcpy(&game_id, received, 8);
    int32_t result;
    memcpy(&result, received + 8, 4);
    CHECK((uint32_t)game_id == (uint32_t)atoi(getenv("SteamAppId") ? getenv("SteamAppId") : "0") && result == 1,
          "UserStatsReceived_t game %llu result %d", (unsigned long long)game_id, result);
    uint32_t achievements = num_achievements(stats);
    printf("info  %u achievements:", achievements);
    for (uint32_t i = 0; i < achievements && i < 12; i++) {
        const char *a = achievement_name(stats, i);
        bool achieved = false;
        get_achievement(stats, a, &achieved);
        printf(" %s%s", a, achieved ? "*" : "");
    }
    printf("\n");
    CHECK(achievements > 0, "achievement list");

    if (argc > 2) {
        const char *a = argv[2];
        bool before = false;
        get_achievement(stats, a, &before);
        const char *title = display_attr(stats, a, "name");
        printf("info  '%s' (%s) achieved before: %d\n", a, title ? title : "?", before);
        CHECK(set_achievement(stats, a), "SetAchievement %s", a);
        CHECK(store_stats(stats), "StoreStats");
        uint8_t stored[24] = {0};
        CHECK(wait_callback(1103, 10, stored, sizeof stored), "UserAchievementStored_t arrived");
        bool after = false;
        CHECK(get_achievement(stats, a, &after) && after, "GetAchievement after unlock: %d", after);
        if (!before) {
            CHECK(clear_achievement(stats, a), "ClearAchievement %s", a);
            CHECK(store_stats(stats), "StoreStats after clear");
            wait_callback(1102, 10, NULL, 0);
            bool cleared = true;
            get_achievement(stats, a, &cleared);
            CHECK(!cleared, "GetAchievement after clear: %d", cleared);
        }
    }

    shutdown();
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
