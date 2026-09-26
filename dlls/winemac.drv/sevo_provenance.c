/*
 * Provenance lines for a game process: which engine, renderer and toolkit
 * answered, and whether a frame ever reached the screen.
 *
 * Copyright 2026 Sevoflurane
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "macdrv.h"
#include "sevo_provenance.h"
#include "sevo_stats.h"

/* How much of a recorded sha256 identifies a renderer DLL in the log. */
#define SEVO_HASH_CHARS 8
/* How often the D3DMetal present-hook counters repeat under SEVO_GFX_LOG=1. */
#define SEVO_COUNTER_INTERVAL_NS (10 * 1000000000ULL)

static int provenance_on;
static int gfx_log_on;
static int layer_off_screen;
static unsigned long long start_ns;

/* The configured renderer and its neighbours, read once at init. The header
   line they fill is held back until a frame is drawn, when the module list
   says which renderer actually answered. */
static char cfg_renderer[64];
static char cfg_toolkit[64];
static char cfg_upscaler[64];
static char cfg_msync[16];
static char cfg_d3d11[80];
static char cfg_d3d12[80];
static char cfg_dxgi[80];
static atomic_int header_printed;
static atomic_int windows_closed_last;

static atomic_ullong present_count;
static atomic_ullong drawable_posted;
static atomic_ullong presented_executed;
static atomic_ullong counters_printed_ns;

static unsigned long long now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void note(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *format, ...)
{
    char line[512];
    va_list args;

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    fprintf(stderr, "sevo:gfx pid=%d %s\n", getpid(), line);
    fflush(stderr);
}

/* The program's own name, as the Dock and the app's run record spell it. */
static void image_basename(char *buffer, size_t size)
{
    const WCHAR *name = RtlGetCurrentPeb()->ProcessParameters->ImagePathName.Buffer, *p;
    size_t i;

    buffer[0] = 0;
    if (!name) return;
    if ((p = wcsrchr(name, '/'))) name = p + 1;
    if ((p = wcsrchr(name, '\\'))) name = p + 1;
    for (i = 0; i + 1 < size && name[i]; i++)
        buffer[i] = (name[i] < 0x20 || name[i] > 0x7e) ? '?' : (char)name[i];
    buffer[i] = 0;
}

/* One value out of the process's Windows environment, which lives in the PEB
   as a UNICODE `NAME=VALUE` block ending in an empty entry. Steam puts
   SteamAppId and SteamGameId there when it launches a game; the unix environ
   the process inherited carries only what the app itself exported. */
static int peb_environment_value(const char *name, char *buffer, size_t size)
{
    const RTL_USER_PROCESS_PARAMETERS *params = RtlGetCurrentPeb()->ProcessParameters;
    size_t name_len = strlen(name);
    const WCHAR *entry;
    size_t i;

    buffer[0] = 0;
    if (!params || !params->Environment) return 0;
    for (entry = params->Environment; *entry; entry += wcslen(entry) + 1)
    {
        for (i = 0; i < name_len; i++)
        {
            WCHAR c = entry[i];

            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != (WCHAR)tolower((unsigned char)name[i])) break;
        }
        if (i < name_len || entry[i] != '=') continue;
        entry += name_len + 1;
        for (i = 0; i + 1 < size && entry[i]; i++)
            buffer[i] = (entry[i] < 0x20 || entry[i] > 0x7e) ? '?' : (char)entry[i];
        buffer[i] = 0;
        return buffer[0] != 0;
    }
    return 0;
}

/* The engine directory this winemac.so was loaded out of. A packaged engine
   is <engine>/wine/lib/wine/<arch>/winemac.so, so the wine root's parent is
   the engine; a staging tree has no such parent and answers with its own
   name. A build tree has no lib/wine at all. */
static int engine_paths(char *engine_dir, size_t dir_size, char *engine_name, size_t name_size)
{
    static const char marker[] = "/lib/wine/";
    Dl_info info;
    const char *path, *at, *found = NULL;
    char root[PATH_MAX], *root_name;
    size_t root_len;

    engine_dir[0] = engine_name[0] = 0;
    if (!dladdr((void *)engine_paths, &info) || !(path = info.dli_fname)) return 0;
    for (at = path; (at = strstr(at, marker)); at++) found = at;
    if (!found) return 0;

    root_len = (size_t)(found - path);
    if (root_len >= sizeof(root)) return 0;
    memcpy(root, path, root_len);
    root[root_len] = 0;

    root_name = strrchr(root, '/');
    if (root_name && !strcmp(root_name, "/wine"))
    {
        *root_name = 0;
        root_name = strrchr(root, '/');
    }
    snprintf(engine_dir, dir_size, "%s", root);
    snprintf(engine_name, name_size, "%s", root_name ? root_name + 1 : root);
    return engine_name[0] != 0;
}

/* One value out of the engine's `renderer-hashes`, which the app writes when
   it stages a renderer into the engine tree: `key=value` lines, `#` comments,
   the keys `renderer` and `toolkit` plus one sha256 per staged DLL under its
   own name (`d3d11`, `d3d12`, `dxgi`). */
static void renderer_record(const char *engine_dir, const char *key, char *out, size_t size)
{
    char path[PATH_MAX], line[512];
    size_t key_len = strlen(key);
    FILE *file;

    snprintf(out, size, "unknown");
    if (!engine_dir[0]) return;
    snprintf(path, sizeof(path), "%s/renderer-hashes", engine_dir);
    if (!(file = fopen(path, "r"))) return;

    while (fgets(line, sizeof(line), file))
    {
        char *p = line, *end;

        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || strncmp(p, key, key_len) || p[key_len] != '=') continue;
        p += key_len + 1;
        end = p + strlen(p);
        while (end > p && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
        if (*p) snprintf(out, size, "%s", p);
        break;
    }
    fclose(file);
}

/* The configured renderer, when the engine has no `renderer-hashes` yet. The
   overrides separate the two renderers that need them; D3DMetal, wined3d and
   auto all run on builtin resolution and are indistinguishable here. */
static void renderer_from_overrides(char *out, size_t size)
{
    const char *overrides = getenv("WINEDLLOVERRIDES");

    if (!overrides) return;
    if (strstr(overrides, "d3d9,d3d10core,d3d11")) snprintf(out, size, "dxvk");
    else if (strstr(overrides, "d3d10core,d3d11")) snprintf(out, size, "dxmt");
}

/* The 32-bit loader entry, whose in-memory list a wow64 game's own d3d DLLs
   live on. winternl.h ships PEB32 / PEB_LDR_DATA32 / LIST_ENTRY32 /
   UNICODE_STRING32 but not this record; toolhelp.c defines its match. */
typedef struct
{
    LIST_ENTRY32     InLoadOrderLinks;
    LIST_ENTRY32     InMemoryOrderLinks;
    LIST_ENTRY32     InInitializationOrderLinks;
    ULONG            DllBase;
    ULONG            EntryPoint;
    ULONG            SizeOfImage;
    UNICODE_STRING32 FullDllName;
    UNICODE_STRING32 BaseDllName;
} SEVO_LDR_ENTRY32;

/* Case-insensitive match of a UTF-16 base name against an ASCII name, whole
   name so `d3d9.dll` does not match `d3d9on12.dll`. */
static int base_name_is(const WCHAR *base, USHORT byte_len, const char *name, size_t len)
{
    size_t i;

    if (!base || byte_len != len * sizeof(WCHAR)) return 0;
    for (i = 0; i < len; i++)
    {
        WCHAR a = base[i];
        char b = name[i];

        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != (WCHAR)(unsigned char)b) return 0;
    }
    return 1;
}

/* Whether a module of this base name is resident on the wow64 process's 32-bit
   loader list. The 32-bit PEB shares this address space, so its ULONG pointers
   dereference directly. A 64-bit process has no WowTebOffset and answers 0. */
static int module_loaded_wow32(const char *name)
{
    const TEB *teb = NtCurrentTeb();
    const TEB32 *teb32;
    const PEB32 *peb32;
    const PEB_LDR_DATA32 *ldr;
    ULONG head, cur;
    size_t len = strlen(name);

    if (!teb->WowTebOffset) return 0;
    teb32 = (const TEB32 *)((const char *)teb + teb->WowTebOffset);
    if (!(peb32 = (const PEB32 *)(ULONG_PTR)teb32->Peb)) return 0;
    if (!(ldr = (const PEB_LDR_DATA32 *)(ULONG_PTR)peb32->LdrData)) return 0;

    head = (ULONG)(ULONG_PTR)&ldr->InMemoryOrderModuleList;
    for (cur = ((const LIST_ENTRY32 *)(ULONG_PTR)head)->Flink; cur && cur != head;
         cur = ((const LIST_ENTRY32 *)(ULONG_PTR)cur)->Flink)
    {
        const SEVO_LDR_ENTRY32 *mod = (const SEVO_LDR_ENTRY32 *)
            (ULONG_PTR)(cur - offsetof(SEVO_LDR_ENTRY32, InMemoryOrderLinks));

        if (base_name_is((const WCHAR *)(ULONG_PTR)mod->BaseDllName.Buffer,
                         mod->BaseDllName.Length, name, len))
            return 1;
    }
    return 0;
}

/* Whether a module of this base name is resident, walking the loader's
   in-memory module list. A 32-bit game loads its d3d DLLs on the 32-bit
   loader list, which the 64-bit list this code runs on does not carry. */
static int module_loaded(const char *name)
{
    const PEB_LDR_DATA *ldr = RtlGetCurrentPeb()->LdrData;
    const LIST_ENTRY *head, *entry;
    size_t len = strlen(name);

    if (ldr)
    {
        head = &ldr->InMemoryOrderModuleList;
        for (entry = head->Flink; entry && entry != head; entry = entry->Flink)
        {
            const LDR_DATA_TABLE_ENTRY *mod =
                CONTAINING_RECORD(entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);

            if (base_name_is(mod->BaseDllName.Buffer, mod->BaseDllName.Length, name, len))
                return 1;
        }
    }
    return module_loaded_wow32(name);
}

/* Whether wined3d resident in the process is the renderer that drew.
   D3DMetal, DXMT and DXVK each supply their own d3d DLLs, so a title that
   loaded one of them for d3d11/d3d12 rendered with it. wined3d beside it is
   Steam's doing: the overlay (gameoverlayrenderer64.dll) loads d3d9.dll into
   every game to hook it, and Wine's d3d9 drags wined3d and opengl32 in with
   it. wined3d is the answer when the configuration names it or names no
   renderer, and when the process holds neither d3d11.dll nor d3d12.dll: a
   D3D9-or-older title in a D3DMetal or DXMT bottle really is on wined3d. */
static int wined3d_answered(void)
{
    if (!module_loaded("wined3d.dll")) return 0;
    if (!strcmp(cfg_renderer, "wined3d") || !strcmp(cfg_renderer, "auto") ||
        !strcmp(cfg_renderer, "unknown"))
        return 1;
    return !module_loaded("d3d11.dll") && !module_loaded("d3d12.dll");
}

/* The renderer that actually answered, from the modules the process loaded.
   When wined3d drew, name its display back end. */
static void actual_renderer(char *out, size_t size)
{
    if (wined3d_answered())
    {
        if (module_loaded("opengl32.dll")) snprintf(out, size, "wined3d-gl");
        else if (module_loaded("winevulkan.dll") || module_loaded("vulkan-1.dll"))
            snprintf(out, size, "wined3d-vulkan");
        else snprintf(out, size, "wined3d");
        return;
    }
    snprintf(out, size, "%s", cfg_renderer);
}

/* The `sevo:gfx` header, emitted once — at the first present, when the module
   list can name the renderer that drew, or at exit for a process that never
   presented a frame. */
static void note_gfx_header(void)
{
    char renderer[64];
    int expected = 0;

    if (!atomic_compare_exchange_strong(&header_printed, &expected, 1)) return;
    actual_renderer(renderer, sizeof(renderer));
    note("renderer=%s toolkit=%s presenter=%s upscaler=%s msync=%s",
         renderer, cfg_toolkit, layer_off_screen ? "on" : "off",
         cfg_upscaler[0] ? cfg_upscaler : "off", cfg_msync);
    note("d3d11=%.*s d3d12=%.*s dxgi=%.*s",
         SEVO_HASH_CHARS, cfg_d3d11, SEVO_HASH_CHARS, cfg_d3d12, SEVO_HASH_CHARS, cfg_dxgi);
}

static void note_counters(void)
{
    note("d3dmetal posted=%llu executed=%llu",
         (unsigned long long)atomic_load(&drawable_posted),
         (unsigned long long)atomic_load(&presented_executed));
}

static void note_exit(void)
{
    note_gfx_header();
    note("exit presents=%llu", (unsigned long long)atomic_load(&present_count));
    if (gfx_log_on) note_counters();
}

void sevo_provenance_init(int presenter_on, const char *upscaler)
{
    char engine_dir[PATH_MAX], engine[NAME_MAX], exe[NAME_MAX], steam_appid[32];
    const char *appid, *msync, *log;

    if (provenance_on) return;
    provenance_on = 1;
    start_ns = now_ns();
    log = getenv("SEVO_GFX_LOG");
    gfx_log_on = log && !strcmp(log, "1");
    layer_off_screen = presenter_on;

    image_basename(exe, sizeof(exe));
    engine_paths(engine_dir, sizeof(engine_dir), engine, sizeof(engine));
    if (!engine[0]) snprintf(engine, sizeof(engine), "unknown");
    if (peb_environment_value("SteamAppId", steam_appid, sizeof(steam_appid)) ||
        peb_environment_value("SteamGameId", steam_appid, sizeof(steam_appid)))
        appid = steam_appid;
    else if (!(appid = getenv("SteamAppId")) && !(appid = getenv("SteamGameId"))) appid = "none";
    if (!(msync = getenv("WINEMSYNC"))) msync = "0";
    snprintf(cfg_msync, sizeof(cfg_msync), "%s", msync);
    snprintf(cfg_upscaler, sizeof(cfg_upscaler), "%s", upscaler && *upscaler ? upscaler : "off");

    renderer_record(engine_dir, "renderer", cfg_renderer, sizeof(cfg_renderer));
    if (!strcmp(cfg_renderer, "unknown")) renderer_from_overrides(cfg_renderer, sizeof(cfg_renderer));
    renderer_record(engine_dir, "toolkit", cfg_toolkit, sizeof(cfg_toolkit));
    renderer_record(engine_dir, "d3d11", cfg_d3d11, sizeof(cfg_d3d11));
    renderer_record(engine_dir, "d3d12", cfg_d3d12, sizeof(cfg_d3d12));
    renderer_record(engine_dir, "dxgi", cfg_dxgi, sizeof(cfg_dxgi));

    sevo_stats_init((unsigned int)strtoul(appid, NULL, 10), exe);

    fprintf(stderr, "sevo:run pid=%d exe=%s appid=%s engine=%s\n",
            getpid(), exe[0] ? exe : "unknown", appid, engine);
    fflush(stderr);

    atexit(note_exit);
}

void sevo_provenance_note_present(const void *surface)
{
    if (!provenance_on) return;
    if (atomic_fetch_add(&present_count, 1) == 0)
    {
        note_gfx_header();
        note("first present +%llums surface=%p layer=%s",
             (now_ns() - start_ns) / 1000000ULL, surface,
             layer_off_screen ? "off-screen" : "on-screen");
    }
}

void sevo_provenance_note_windows_closed(void)
{
    if (!provenance_on || !atomic_load(&present_count)) return;
    atomic_store(&windows_closed_last, 1);
    fprintf(stderr, "sevo:exit pid=%d wpid=%04x windows closed\n",
            getpid(), (unsigned int)GetCurrentProcessId());
    fflush(stderr);
}

void sevo_provenance_note_windows_shown(void)
{
    int expected = 1;

    if (!atomic_compare_exchange_strong(&windows_closed_last, &expected, 0)) return;
    fprintf(stderr, "sevo:exit pid=%d wpid=%04x windows reopened\n",
            getpid(), (unsigned int)GetCurrentProcessId());
    fflush(stderr);
}

void sevo_provenance_note_drawable(void)
{
    atomic_fetch_add(&drawable_posted, 1);
}

void sevo_provenance_note_presented_event(void)
{
    unsigned long long executed, now, last;

    executed = atomic_fetch_add(&presented_executed, 1) + 1;
    if (!gfx_log_on) return;
    now = now_ns();
    last = atomic_load(&counters_printed_ns);
    if (executed > 1 && now - last < SEVO_COUNTER_INTERVAL_NS) return;
    atomic_store(&counters_printed_ns, now);
    note_counters();
}
