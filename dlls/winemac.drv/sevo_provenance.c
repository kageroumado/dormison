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

/* How much of a recorded sha256 identifies a renderer DLL in the log. */
#define SEVO_HASH_CHARS 8
/* How often the D3DMetal present-hook counters repeat under SEVO_GFX_LOG=1. */
#define SEVO_COUNTER_INTERVAL_NS (10 * 1000000000ULL)

static int provenance_on;
static int gfx_log_on;
static int layer_off_screen;
static unsigned long long start_ns;

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

/* Which renderer answered, when the engine has no `renderer-hashes` yet. The
   overrides separate the two renderers that need them; D3DMetal, wined3d and
   auto all run on builtin resolution and are indistinguishable here. */
static void renderer_from_overrides(char *out, size_t size)
{
    const char *overrides = getenv("WINEDLLOVERRIDES");

    if (!overrides) return;
    if (strstr(overrides, "d3d9,d3d10core,d3d11")) snprintf(out, size, "dxvk");
    else if (strstr(overrides, "d3d10core,d3d11")) snprintf(out, size, "dxmt");
}

static void note_counters(void)
{
    note("d3dmetal posted=%llu executed=%llu",
         (unsigned long long)atomic_load(&drawable_posted),
         (unsigned long long)atomic_load(&presented_executed));
}

static void note_exit(void)
{
    note("exit presents=%llu", (unsigned long long)atomic_load(&present_count));
    if (gfx_log_on) note_counters();
}

void sevo_provenance_init(int presenter_on, const char *upscaler)
{
    char engine_dir[PATH_MAX], engine[NAME_MAX], exe[NAME_MAX];
    char renderer[64], toolkit[64], d3d11[80], d3d12[80], dxgi[80];
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
    if (!(appid = getenv("SteamAppId")) && !(appid = getenv("SteamGameId"))) appid = "none";
    if (!(msync = getenv("WINEMSYNC"))) msync = "0";

    renderer_record(engine_dir, "renderer", renderer, sizeof(renderer));
    if (!strcmp(renderer, "unknown")) renderer_from_overrides(renderer, sizeof(renderer));
    renderer_record(engine_dir, "toolkit", toolkit, sizeof(toolkit));
    renderer_record(engine_dir, "d3d11", d3d11, sizeof(d3d11));
    renderer_record(engine_dir, "d3d12", d3d12, sizeof(d3d12));
    renderer_record(engine_dir, "dxgi", dxgi, sizeof(dxgi));

    fprintf(stderr, "sevo:run pid=%d exe=%s appid=%s engine=%s\n",
            getpid(), exe[0] ? exe : "unknown", appid, engine);
    fflush(stderr);
    note("renderer=%s toolkit=%s presenter=%s upscaler=%s msync=%s",
         renderer, toolkit, presenter_on ? "on" : "off",
         upscaler && *upscaler ? upscaler : "off", msync);
    note("d3d11=%.*s d3d12=%.*s dxgi=%.*s",
         SEVO_HASH_CHARS, d3d11, SEVO_HASH_CHARS, d3d12, SEVO_HASH_CHARS, dxgi);

    atexit(note_exit);
}

void sevo_provenance_note_present(const void *surface)
{
    if (!provenance_on) return;
    if (atomic_fetch_add(&present_count, 1) == 0)
        note("first present +%llums surface=%p layer=%s",
             (now_ns() - start_ns) / 1000000ULL, surface,
             layer_off_screen ? "off-screen" : "on-screen");
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
