/*
 * sevo-steamstub.exe — the Steamworks half of a game that has left the bottle.
 *
 * When the app runs an NW.js game in native macOS NW.js, the native process
 * cannot reach the bottle's steam_api.dll. This stub stays inside the bottle,
 * holds the Steamworks connection on the game's behalf, and answers
 * newline-delimited JSON on 127.0.0.1. The native side talks to it through
 * the app's greenworks.js.
 *
 * Build: make            (produces sevo-steamstub.exe and sevo-steamstub32.exe)
 *
 * Environment:
 *   SteamAppId              the app id Steamworks initializes against (required)
 *   SEVO_STEAM_API_DIR      directory holding steam_api64.dll / steam_api.dll
 *   SEVO_STEAM_STUB_PORT    listening port, default 27060
 *   SEVO_STEAM_STUB_IDLE    seconds to wait for a first client, default 300
 * argv[1]                   game directory, searched for the dll when
 *                           SEVO_STEAM_API_DIR is unset
 *
 * The chosen port is written to
 *   %LOCALAPPDATA%\Sevoflurane\steamstub-<appid>.port
 * and a transcript to steamstub-<appid>.log beside it.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PORT 27060
#define DEFAULT_IDLE_SECONDS 300
#define MAX_CLIENTS 4
#define LINE_MAX 8192

/* Set in the environment of the sibling stub this one hands an invocation to,
 * so the sibling never hands it back: a dll neither bitness loads ends with
 * one log line, where the two stubs re-executed each other without end. */
#define HANDOFF_VARIABLE "SEVO_STEAM_STUB_HANDOFF"

/* The PE machine of this build and of the sibling build. */
#if defined(_WIN64)
#define OWN_MACHINE IMAGE_FILE_MACHINE_AMD64
#define SIBLING_MACHINE IMAGE_FILE_MACHINE_I386
#else
#define OWN_MACHINE IMAGE_FILE_MACHINE_I386
#define SIBLING_MACHINE IMAGE_FILE_MACHINE_AMD64
#endif

/* ------------------------------------------------------------------ log */

static FILE *g_log;

static void logf_(const char *fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* --------------------------------------------------- Steamworks flat API */

typedef void ISteamUserStats;
typedef void ISteamUser;
typedef void ISteamApps;
typedef void ISteamFriends;

typedef int (__cdecl *fn_InitFlat)(char *err);
typedef unsigned char (__cdecl *fn_Init)(void);
typedef void (__cdecl *fn_Void)(void);
typedef ISteamUserStats *(__cdecl *fn_StatsAccessor)(void);
typedef ISteamUser *(__cdecl *fn_UserAccessor)(void);
typedef ISteamApps *(__cdecl *fn_AppsAccessor)(void);
typedef ISteamFriends *(__cdecl *fn_FriendsAccessor)(void);
typedef unsigned char (__cdecl *fn_StatsName)(ISteamUserStats *, const char *);
typedef unsigned char (__cdecl *fn_GetAchievement)(ISteamUserStats *, const char *, unsigned char *);
typedef unsigned char (__cdecl *fn_StoreStats)(ISteamUserStats *);
typedef unsigned int (__cdecl *fn_GetNumAchievements)(ISteamUserStats *);
typedef const char *(__cdecl *fn_GetAchievementName)(ISteamUserStats *, unsigned int);
typedef unsigned char (__cdecl *fn_SetStatInt32)(ISteamUserStats *, const char *, int);
typedef unsigned char (__cdecl *fn_GetStatInt32)(ISteamUserStats *, const char *, int *);
typedef unsigned char (__cdecl *fn_SetStatFloat)(ISteamUserStats *, const char *, float);
typedef unsigned char (__cdecl *fn_GetStatFloat)(ISteamUserStats *, const char *, float *);
typedef unsigned long long (__cdecl *fn_GetSteamID)(ISteamUser *);
typedef const char *(__cdecl *fn_GetLanguage)(ISteamApps *);
typedef const char *(__cdecl *fn_GetPersonaName)(ISteamFriends *);

static struct {
    HMODULE dll;
    fn_Void run_callbacks;
    fn_Void shutdown;
    ISteamUserStats *stats;
    ISteamUser *user;
    ISteamApps *apps;
    ISteamFriends *friends;
    fn_StatsName set_achievement;
    fn_GetAchievement get_achievement;
    fn_StatsName clear_achievement;
    fn_StoreStats store_stats;
    fn_Void request_stats;          /* RequestCurrentStats, called through a cast */
    fn_GetNumAchievements num_achievements;
    fn_GetAchievementName achievement_name;
    fn_SetStatInt32 set_stat_i;
    fn_GetStatInt32 get_stat_i;
    fn_SetStatFloat set_stat_f;
    fn_GetStatFloat get_stat_f;
    fn_GetSteamID get_steam_id;
    fn_GetLanguage get_language;
    fn_GetPersonaName get_persona_name;
} S;

/* Steamworks versions its interface accessors into the export name
 * (SteamAPI_SteamUserStats_v012) and bumps the number with the SDK, so the one
 * a given game shipped is unknown until the dll is open. The walk goes newest
 * first and takes the first name that resolves. */
static FARPROC resolve_accessor(HMODULE dll, const char *base) {
    char name[128];
    for (int v = 40; v >= 1; --v) {
        snprintf(name, sizeof name, "%s_v%03d", base, v);
        FARPROC p = GetProcAddress(dll, name);
        if (p) {
            logf_("accessor %s", name);
            return p;
        }
    }
    FARPROC p = GetProcAddress(dll, base);
    if (p) logf_("accessor %s", base);
    return p;
}

static FARPROC need(HMODULE dll, const char *name) {
    FARPROC p = GetProcAddress(dll, name);
    if (!p) logf_("missing export %s", name);
    return p;
}

/* ------------------------------------------------------- dll discovery */

static int file_exists(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* The machine field of a PE's file header (IMAGE_FILE_MACHINE_AMD64 or
 * IMAGE_FILE_MACHINE_I386 for a Steamworks dll), or 0 for a file that is not
 * a PE. Read from the file, so the bitness is known before any LoadLibrary. */
static WORD pe_machine(const char *path) {
    WORD machine = 0;
    IMAGE_DOS_HEADER dos;
    DWORD signature;
    IMAGE_FILE_HEADER header;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fread(&dos, sizeof dos, 1, f) == 1 && dos.e_magic == IMAGE_DOS_SIGNATURE
        && fseek(f, dos.e_lfanew, SEEK_SET) == 0
        && fread(&signature, sizeof signature, 1, f) == 1 && signature == IMAGE_NT_SIGNATURE
        && fread(&header, sizeof header, 1, f) == 1)
        machine = header.Machine;
    fclose(f);
    return machine;
}

/* Games scatter steam_api next to the exe, under www/, or beside the
 * greenworks .node file, so a shallow walk beats a fixed list. Within one
 * directory the dll of this build's own bitness comes first, so the stub that
 * was handed an invocation finds the one it can load. */
static int find_dll(const char *dir, int depth, char *out, size_t out_size) {
    static const char *names[] = {
#if defined(_WIN64)
        "steam_api64.dll", "steam_api.dll"
#else
        "steam_api.dll", "steam_api64.dll"
#endif
    };
    for (int i = 0; i < 2; ++i) {
        snprintf(out, out_size, "%s\\%s", dir, names[i]);
        if (file_exists(out)) return 1;
    }
    if (depth <= 0) return 0;

    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        char sub[MAX_PATH];
        snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
        if (find_dll(sub, depth - 1, out, out_size)) { found = 1; break; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

/* A 64-bit stub cannot load a 32-bit steam_api.dll, and RPG Maker MV ships
 * 32-bit. The whole invocation goes to the sibling build, which answers. The
 * sibling inherits HANDOFF_VARIABLE and so hands nothing back. */
static int reexec_other_bitness(int argc, char **argv) {
    char self[MAX_PATH];
    if (!GetModuleFileNameA(NULL, self, sizeof self)) return 0;
    char *slash = strrchr(self, '\\');
    if (!slash) return 0;
    *slash = 0;

    char peer[MAX_PATH];
    snprintf(peer, sizeof peer, "%s\\%s", self,
             sizeof(void *) == 8 ? "sevo-steamstub32.exe" : "sevo-steamstub.exe");
    if (!file_exists(peer)) {
        logf_("no sibling stub at %s", peer);
        return 0;
    }

    char cmd[LINE_MAX];
    int n = snprintf(cmd, sizeof cmd, "\"%s\"", peer);
    for (int i = 1; i < argc && n > 0 && n < (int)sizeof cmd; ++i)
        n += snprintf(cmd + n, sizeof cmd - n, " \"%s\"", argv[i]);

    SetEnvironmentVariableA(HANDOFF_VARIABLE, "1");
    STARTUPINFOA si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        logf_("CreateProcess %s failed: %lu", peer, GetLastError());
        return 0;
    }
    logf_("handed off to %s", peer);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* ------------------------------------------------------------ tiny JSON */

/* The client speaks one flat object per line — {"op":…,"name":…,"value":…} —
 * so a scanner for that shape replaces a parser. */
static int json_string(const char *line, const char *key, char *out, size_t out_size) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') ++p;
    if (*p != '"') return 0;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_size) {
        if (*p == '\\' && p[1]) {
            ++p;
            char c = *p++;
            out[i++] = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c;
            continue;
        }
        out[i++] = *p++;
    }
    out[i] = 0;
    return *p == '"';
}

static int json_number(const char *line, const char *key, double *out) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') ++p;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return 0;
    *out = v;
    return 1;
}

static void json_escape(const char *in, char *out, size_t out_size) {
    size_t i = 0;
    for (; *in && i + 7 < out_size; ++in) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[i++] = '\\'; out[i++] = (char)c; }
        else if (c == '\n') { out[i++] = '\\'; out[i++] = 'n'; }
        else if (c == '\r') { out[i++] = '\\'; out[i++] = 'r'; }
        else if (c == '\t') { out[i++] = '\\'; out[i++] = 't'; }
        else if (c < 0x20) i += snprintf(out + i, out_size - i, "\\u%04x", c);
        else out[i++] = (char)c;
    }
    out[i] = 0;
}

/* ---------------------------------------------------------------- state */

static unsigned int g_appid;
static char g_port_path[MAX_PATH];

/* ----------------------------------------------------------- stat types */

/* A stat is INT or FLOAT in the app's Steamworks schema, and Valve's setters
 * succeed only for the matching type. The wire carries a name and a number,
 * so the schema is read from Steamworks itself: the typed getters answer
 * true only for a stat of their type. Answers are kept per name, since the
 * schema does not change while the stub runs. */
enum stat_type { STAT_UNKNOWN, STAT_INT32, STAT_FLOAT };

#define STAT_TYPE_CACHE 64
#define STAT_NAME_MAX 128

static struct {
    char name[STAT_NAME_MAX];
    enum stat_type type;
} g_stat_types[STAT_TYPE_CACHE];
static int g_stat_type_count;

static const char *stat_type_name(enum stat_type type) {
    return type == STAT_INT32 ? "INT32" : type == STAT_FLOAT ? "FLOAT" : "unknown";
}

static enum stat_type stat_type_of(const char *name) {
    for (int i = 0; i < g_stat_type_count; ++i)
        if (!strcmp(g_stat_types[i].name, name)) return g_stat_types[i].type;

    enum stat_type type = STAT_UNKNOWN;
    int as_int = 0;
    float as_float = 0;
    if (S.get_stat_i && S.get_stat_i(S.stats, name, &as_int)) type = STAT_INT32;
    else if (S.get_stat_f && S.get_stat_f(S.stats, name, &as_float)) type = STAT_FLOAT;

    /* An unknown type is asked again next time: the stats may not have
     * arrived from Steam yet. */
    if (type != STAT_UNKNOWN && g_stat_type_count < STAT_TYPE_CACHE && strlen(name) < STAT_NAME_MAX) {
        strcpy(g_stat_types[g_stat_type_count].name, name);
        g_stat_types[g_stat_type_count].type = type;
        ++g_stat_type_count;
    }
    return type;
}

static void send_line(SOCKET s, const char *text) {
    size_t len = strlen(text);
    send(s, text, (int)len, 0);
    send(s, "\n", 1, 0);
}

static void send_error(SOCKET s, const char *text) {
    char esc[1024], buf[1200];
    json_escape(text, esc, sizeof esc);
    snprintf(buf, sizeof buf, "{\"ok\":false,\"error\":\"%s\"}", esc);
    send_line(s, buf);
}

/* Returns 1 when the client asked the stub to exit. */
static int handle_line(SOCKET s, const char *line) {
    char op[64] = "", name[512] = "";
    if (!json_string(line, "op", op, sizeof op)) {
        send_error(s, "no op");
        return 0;
    }
    int has_name = json_string(line, "name", name, sizeof name);
    double value = 0;
    int has_value = json_number(line, "value", &value);
    logf_("op %s name=%s", op, has_name ? name : "-");

    if (!strcmp(op, "ping")) {
        send_line(s, "{\"ok\":true}");
        return 0;
    }

    if (!strcmp(op, "quit")) {
        send_line(s, "{\"ok\":true}");
        return 1;
    }

    if (!strcmp(op, "init")) {
        unsigned long long id = S.get_steam_id && S.user ? S.get_steam_id(S.user) : 0;
        const char *lang = S.get_language && S.apps ? S.get_language(S.apps) : "";
        const char *who = S.get_persona_name && S.friends ? S.get_persona_name(S.friends) : "";
        char esc[128], esc_who[512], buf[1024];
        json_escape(lang ? lang : "", esc, sizeof esc);
        json_escape(who ? who : "", esc_who, sizeof esc_who);
        snprintf(buf, sizeof buf,
                 "{\"ok\":true,\"steamId\":\"%llu\",\"appId\":%u,"
                 "\"language\":\"%s\",\"personaName\":\"%s\"}",
                 id, g_appid, esc, esc_who);
        send_line(s, buf);
        return 0;
    }

    if (!S.stats) {
        send_error(s, "ISteamUserStats unavailable");
        return 0;
    }

    if (!strcmp(op, "activateAchievement")) {
        if (!has_name) { send_error(s, "no name"); return 0; }
        if (!S.set_achievement || !S.set_achievement(S.stats, name)) { send_error(s, "SetAchievement failed"); return 0; }
        if (!S.store_stats || !S.store_stats(S.stats)) { send_error(s, "StoreStats failed"); return 0; }
        send_line(s, "{\"ok\":true}");
        return 0;
    }

    if (!strcmp(op, "clearAchievement")) {
        if (!has_name) { send_error(s, "no name"); return 0; }
        if (!S.clear_achievement || !S.clear_achievement(S.stats, name)) { send_error(s, "ClearAchievement failed"); return 0; }
        if (!S.store_stats || !S.store_stats(S.stats)) { send_error(s, "StoreStats failed"); return 0; }
        send_line(s, "{\"ok\":true}");
        return 0;
    }

    if (!strcmp(op, "getAchievement")) {
        if (!has_name) { send_error(s, "no name"); return 0; }
        unsigned char achieved = 0;
        if (!S.get_achievement || !S.get_achievement(S.stats, name, &achieved)) { send_error(s, "GetAchievement failed"); return 0; }
        char buf[64];
        snprintf(buf, sizeof buf, "{\"ok\":true,\"achieved\":%s}", achieved ? "true" : "false");
        send_line(s, buf);
        return 0;
    }

    if (!strcmp(op, "getAchievementNames")) {
        unsigned int count = S.num_achievements ? S.num_achievements(S.stats) : 0;
        char *buf = malloc((size_t)count * 530 + 64);
        if (!buf) { send_error(s, "out of memory"); return 0; }
        int n = snprintf(buf, 64, "{\"ok\":true,\"names\":[");
        for (unsigned int i = 0; i < count; ++i) {
            const char *an = S.achievement_name ? S.achievement_name(S.stats, i) : NULL;
            char esc[520];
            json_escape(an ? an : "", esc, sizeof esc);
            n += sprintf(buf + n, "%s\"%s\"", i ? "," : "", esc);
        }
        sprintf(buf + n, "]}");
        send_line(s, buf);
        free(buf);
        return 0;
    }

    if (!strcmp(op, "getNumberOfAchievements")) {
        char buf[64];
        snprintf(buf, sizeof buf, "{\"ok\":true,\"count\":%u}",
                 S.num_achievements ? S.num_achievements(S.stats) : 0);
        send_line(s, buf);
        return 0;
    }

    if (!strcmp(op, "setStat")) {
        if (!has_name || !has_value) { send_error(s, "no name or value"); return 0; }
        if (!isfinite(value)) { send_error(s, "value is not a finite number"); return 0; }
        enum stat_type type = stat_type_of(name);
        if (type == STAT_UNKNOWN) {
            type = value == floor(value) && fabs(value) <= INT_MAX ? STAT_INT32 : STAT_FLOAT;
            logf_("stat %s: neither typed getter answers, so its type is guessed as %s from the value %g",
                  name, stat_type_name(type), value);
        }
        int ok;
        if (type == STAT_INT32) {
            if (value < INT_MIN || value > INT_MAX) { send_error(s, "value is outside the INT32 stat's range"); return 0; }
            if (value != floor(value)) logf_("stat %s is INT32; %g is truncated to %d", name, value, (int)value);
            ok = S.set_stat_i && S.set_stat_i(S.stats, name, (int)value);
        } else {
            if (fabs(value) > FLT_MAX) { send_error(s, "value is outside the FLOAT stat's range"); return 0; }
            ok = S.set_stat_f && S.set_stat_f(S.stats, name, (float)value);
        }
        if (!ok) {
            char text[256];
            snprintf(text, sizeof text, "SetStat%s failed", stat_type_name(type));
            send_error(s, text);
            return 0;
        }
        send_line(s, "{\"ok\":true}");
        return 0;
    }

    if (!strcmp(op, "getStat") || !strcmp(op, "getStatInt")) {
        if (!has_name) { send_error(s, "no name"); return 0; }
        int v = 0;
        if (!S.get_stat_i || !S.get_stat_i(S.stats, name, &v)) { send_error(s, "GetStatInt32 failed"); return 0; }
        char buf[64];
        snprintf(buf, sizeof buf, "{\"ok\":true,\"value\":%d}", v);
        send_line(s, buf);
        return 0;
    }

    if (!strcmp(op, "getStatFloat")) {
        if (!has_name) { send_error(s, "no name"); return 0; }
        float v = 0;
        if (!S.get_stat_f || !S.get_stat_f(S.stats, name, &v)) { send_error(s, "GetStatFloat failed"); return 0; }
        char buf[64];
        snprintf(buf, sizeof buf, "{\"ok\":true,\"value\":%.6f}", v);
        send_line(s, buf);
        return 0;
    }

    if (!strcmp(op, "storeStats")) {
        if (!S.store_stats || !S.store_stats(S.stats)) { send_error(s, "StoreStats failed"); return 0; }
        send_line(s, "{\"ok\":true}");
        return 0;
    }

    send_error(s, "unknown op");
    return 0;
}

/* ----------------------------------------------------------- port file */

static void local_appdata_dir(char *out, size_t out_size) {
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !*base) base = "C:\\";
    snprintf(out, out_size, "%s\\Sevoflurane", base);
    CreateDirectoryA(out, NULL);
}

static void write_port_file(unsigned short port) {
    FILE *f = fopen(g_port_path, "w");
    if (!f) { logf_("cannot write %s", g_port_path); return; }
    fprintf(f, "%u\n", port);
    fclose(f);
    logf_("port %u written to %s", port, g_port_path);
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv) {
    char dir[MAX_PATH];
    local_appdata_dir(dir, sizeof dir);

    const char *appid_env = getenv("SteamAppId");
    g_appid = appid_env ? (unsigned int)strtoul(appid_env, NULL, 10) : 0;

    /* A stub that was handed an invocation appends, so the transcript keeps
     * the hand-off line the first stub wrote. */
    char log_path[MAX_PATH];
    snprintf(log_path, sizeof log_path, "%s\\steamstub-%u.log", dir, g_appid);
    g_log = fopen(log_path, getenv(HANDOFF_VARIABLE) ? "a" : "w");
    snprintf(g_port_path, sizeof g_port_path, "%s\\steamstub-%u.port", dir, g_appid);

    logf_("sevo-steamstub %d-bit, appid %u", (int)(sizeof(void *) * 8), g_appid);
    if (!g_appid) {
        logf_("SteamAppId unset; SteamAPI_Init will fail");
    }

    /* Find and open the game's Steamworks dll. */
    const char *search = getenv("SEVO_STEAM_API_DIR");
    if (!search || !*search) search = argc > 1 ? argv[1] : ".";
    char dll_path[MAX_PATH];
    if (!find_dll(search, 4, dll_path, sizeof dll_path)) {
        logf_("no steam_api dll under %s", search);
        return 2;
    }
    /* The dll's bitness is read from its header and decided once: the
     * sibling's dll is handed to the sibling, a dll of neither bitness ends
     * here with the machine named, and a stub that was handed an invocation
     * never hands it back. */
    WORD machine = pe_machine(dll_path);
    if (machine == SIBLING_MACHINE) {
        if (getenv(HANDOFF_VARIABLE)) {
            logf_("%s has PE machine 0x%04x, which the stub that handed off to this one owns; giving up",
                  dll_path, machine);
            return 3;
        }
        return reexec_other_bitness(argc, argv) ? 0 : 3;
    }
    if (machine != OWN_MACHINE) {
        logf_("%s has PE machine 0x%04x, which neither stub loads", dll_path, machine);
        return 3;
    }
    logf_("loading %s", dll_path);

    S.dll = LoadLibraryA(dll_path);
    if (!S.dll) {
        logf_("LoadLibrary failed: %lu", GetLastError());
        return 3;
    }

    fn_InitFlat init_flat = (fn_InitFlat)GetProcAddress(S.dll, "SteamAPI_InitFlat");
    fn_Init init_plain = (fn_Init)GetProcAddress(S.dll, "SteamAPI_Init");
    if (init_flat) {
        char err[1024] = "";
        int rc = init_flat(err);
        if (rc != 0) { logf_("SteamAPI_InitFlat -> %d: %s", rc, err); return 4; }
    } else if (init_plain) {
        if (!init_plain()) { logf_("SteamAPI_Init returned false"); return 4; }
    } else {
        logf_("neither SteamAPI_Init nor SteamAPI_InitFlat exported");
        return 4;
    }
    logf_("Steamworks initialized");

    S.run_callbacks = (fn_Void)need(S.dll, "SteamAPI_RunCallbacks");
    S.shutdown = (fn_Void)need(S.dll, "SteamAPI_Shutdown");

    fn_StatsAccessor stats_accessor = (fn_StatsAccessor)resolve_accessor(S.dll, "SteamAPI_SteamUserStats");
    fn_UserAccessor user_accessor = (fn_UserAccessor)resolve_accessor(S.dll, "SteamAPI_SteamUser");
    fn_AppsAccessor apps_accessor = (fn_AppsAccessor)resolve_accessor(S.dll, "SteamAPI_SteamApps");
    fn_FriendsAccessor friends_accessor = (fn_FriendsAccessor)resolve_accessor(S.dll, "SteamAPI_SteamFriends");
    S.stats = stats_accessor ? stats_accessor() : NULL;
    S.user = user_accessor ? user_accessor() : NULL;
    S.apps = apps_accessor ? apps_accessor() : NULL;
    S.friends = friends_accessor ? friends_accessor() : NULL;

    S.set_achievement = (fn_StatsName)need(S.dll, "SteamAPI_ISteamUserStats_SetAchievement");
    S.get_achievement = (fn_GetAchievement)need(S.dll, "SteamAPI_ISteamUserStats_GetAchievement");
    S.clear_achievement = (fn_StatsName)need(S.dll, "SteamAPI_ISteamUserStats_ClearAchievement");
    S.store_stats = (fn_StoreStats)need(S.dll, "SteamAPI_ISteamUserStats_StoreStats");
    S.request_stats = (fn_Void)GetProcAddress(S.dll, "SteamAPI_ISteamUserStats_RequestCurrentStats");
    S.num_achievements = (fn_GetNumAchievements)need(S.dll, "SteamAPI_ISteamUserStats_GetNumAchievements");
    S.achievement_name = (fn_GetAchievementName)need(S.dll, "SteamAPI_ISteamUserStats_GetAchievementName");
    S.set_stat_i = (fn_SetStatInt32)need(S.dll, "SteamAPI_ISteamUserStats_SetStatInt32");
    S.get_stat_i = (fn_GetStatInt32)need(S.dll, "SteamAPI_ISteamUserStats_GetStatInt32");
    S.set_stat_f = (fn_SetStatFloat)GetProcAddress(S.dll, "SteamAPI_ISteamUserStats_SetStatFloat");
    S.get_stat_f = (fn_GetStatFloat)GetProcAddress(S.dll, "SteamAPI_ISteamUserStats_GetStatFloat");
    S.get_steam_id = (fn_GetSteamID)need(S.dll, "SteamAPI_ISteamUser_GetSteamID");
    S.get_language = (fn_GetLanguage)need(S.dll, "SteamAPI_ISteamApps_GetCurrentGameLanguage");
    S.get_persona_name = (fn_GetPersonaName)GetProcAddress(S.dll, "SteamAPI_ISteamFriends_GetPersonaName");

    /* Achievement and stat values arrive with the first callback pass, so ask
     * for them before anyone can query and pump until they land. */
    if (S.request_stats && S.stats) ((unsigned char (__cdecl *)(ISteamUserStats *))S.request_stats)(S.stats);
    for (int i = 0; i < 10 && S.run_callbacks; ++i) { S.run_callbacks(); Sleep(50); }

    /* Listen. */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { logf_("WSAStartup failed"); return 5; }

    const char *port_env = getenv("SEVO_STEAM_STUB_PORT");
    unsigned short want_port = port_env && *port_env
        ? (unsigned short)strtoul(port_env, NULL, 10) : DEFAULT_PORT;

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { logf_("socket failed"); return 5; }
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(want_port) };
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR) {
        logf_("port %u busy (%d), taking an ephemeral one", want_port, WSAGetLastError());
        addr.sin_port = 0;
        if (bind(listener, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR) {
            logf_("bind failed: %d", WSAGetLastError());
            return 5;
        }
    }
    if (listen(listener, 4) == SOCKET_ERROR) { logf_("listen failed: %d", WSAGetLastError()); return 5; }

    struct sockaddr_in bound;
    int bound_len = sizeof bound;
    getsockname(listener, (struct sockaddr *)&bound, &bound_len);
    write_port_file(ntohs(bound.sin_port));

    const char *idle_env = getenv("SEVO_STEAM_STUB_IDLE");
    DWORD idle_ms = 1000 * (idle_env && *idle_env
        ? (DWORD)strtoul(idle_env, NULL, 10) : DEFAULT_IDLE_SECONDS);

    SOCKET clients[MAX_CLIENTS];
    char pending[MAX_CLIENTS][LINE_MAX];
    size_t pending_len[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; ++i) { clients[i] = INVALID_SOCKET; pending_len[i] = 0; }

    int ever_connected = 0, live = 0, quitting = 0;
    DWORD started = GetTickCount();

    while (!quitting) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(listener, &rd);
        for (int i = 0; i < MAX_CLIENTS; ++i)
            if (clients[i] != INVALID_SOCKET) FD_SET(clients[i], &rd);

        /* The 100 ms tick is the callback pump as much as it is a poll. */
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100 * 1000 };
        int ready = select(0, &rd, NULL, NULL, &tv);
        if (S.run_callbacks) S.run_callbacks();

        if (!ever_connected && GetTickCount() - started > idle_ms) {
            logf_("no client within %lu s, exiting", idle_ms / 1000);
            break;
        }
        if (ready <= 0) continue;

        if (FD_ISSET(listener, &rd)) {
            SOCKET c = accept(listener, NULL, NULL);
            if (c != INVALID_SOCKET) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; ++i) if (clients[i] == INVALID_SOCKET) { slot = i; break; }
                if (slot < 0) { closesocket(c); }
                else {
                    clients[slot] = c;
                    pending_len[slot] = 0;
                    ever_connected = 1;
                    ++live;
                    logf_("client %d connected", slot);
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS && !quitting; ++i) {
            if (clients[i] == INVALID_SOCKET || !FD_ISSET(clients[i], &rd)) continue;
            char chunk[2048];
            int n = recv(clients[i], chunk, sizeof chunk, 0);
            if (n <= 0) {
                logf_("client %d disconnected", i);
                closesocket(clients[i]);
                clients[i] = INVALID_SOCKET;
                --live;
                continue;
            }
            for (int k = 0; k < n && !quitting; ++k) {
                char ch = chunk[k];
                if (ch == '\n') {
                    pending[i][pending_len[i]] = 0;
                    if (pending_len[i]) quitting = handle_line(clients[i], pending[i]);
                    pending_len[i] = 0;
                } else if (pending_len[i] + 1 < LINE_MAX) {
                    pending[i][pending_len[i]++] = ch;
                }
            }
        }

        /* The game is the only client; when it lets go, the stub's work is done. */
        if (ever_connected && live == 0) {
            logf_("last client gone, exiting");
            break;
        }
    }

    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (clients[i] != INVALID_SOCKET) closesocket(clients[i]);
    closesocket(listener);
    WSACleanup();
    DeleteFileA(g_port_path);
    if (S.shutdown) S.shutdown();
    logf_("exit");
    if (g_log) fclose(g_log);
    return 0;
}
