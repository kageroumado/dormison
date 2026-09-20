/*
 * The present counter: a page of shared memory per process that the app
 * samples to get a frame rate and a stall signal without hooking the game.
 *
 * Copyright 2026 kageroumado
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

#ifndef __WINE_SEVO_STATS_H
#define __WINE_SEVO_STATS_H

#include <stdint.h>

/* `SEVOSTS1`, stored last so a reader never meets a half-written page. */
#define SEVO_STATS_MAGIC     0x5345564f53545331ULL
#define SEVO_STATS_VERSION   1
#define SEVO_STATS_PAGE_SIZE 4096

/* Which present path the frame counter belongs to. Both fire once per frame,
   so either gives the same rate; the presenter wins because it owns the
   screen wherever it runs, and because a GDI window has no client surface at
   all. */
enum sevo_stats_source
{
    SEVO_STATS_SOURCE_NONE           = 0,
    SEVO_STATS_SOURCE_CLIENT_SURFACE = 1,
    SEVO_STATS_SOURCE_PRESENTER      = 2,
};

/* The page at <prefix>/.sevo/run/<pid>.stats, as the app reads it.
   Times are `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)`: nanoseconds in both
   the x86_64 process that writes them and the arm64 app that reads them,
   where the raw mach tick is a different unit on each side. */
struct sevo_stats_page
{
    uint64_t magic;
    /* Presents that reached the screen, counted on one path (`source`).
       One per frame for Vulkan, OpenGL and the presenter. For D3DMetal it is
       the CLIENT_SURFACE_PRESENTED event, and the event queue holds one of
       those per client surface at a time, so a game presenting faster than
       the driver drains its queue has its presents coalesced here. */
    uint64_t frames;
    /* Drawables D3DMetal took: the frames the game produced, one per
       `-[WineMetalLayer nextDrawable]` and never coalesced, which is the
       frame rate to read whenever it is non-zero. */
    uint64_t drawables;
    uint64_t last_present_ns;
    uint64_t start_ns;
    /* The Cocoa window number of the window being presented into, 0 until a
       present on a path that knows it. */
    uint64_t window_id;
    uint32_t version;
    uint32_t pid;
    uint32_t source;
    /* The Steam app id, 0 when the process has none. */
    uint32_t appid;
    char     exe[32];
};

/* Names the process for the page it will make. Called once, before any
   present; the page itself is made by the first present, so a process that
   never draws leaves no file. */
extern void sevo_stats_init(unsigned int appid, const char *exe);
extern unsigned int sevo_stats_appid(void);

extern void sevo_stats_note_present(unsigned int source);
extern void sevo_stats_note_drawable(void);
/* The first window number a present path can name; later calls are ignored. */
extern void sevo_stats_note_window(unsigned long long window_id);

#endif  /* __WINE_SEVO_STATS_H */
