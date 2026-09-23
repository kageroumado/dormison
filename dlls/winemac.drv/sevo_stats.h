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
/* Frame timestamps the page holds: what is left of the page after the fields before it. */
#define SEVO_STATS_RING_CAPACITY 994

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
    /* When the Cocoa main thread's run loop last turned, written once a
       second by a timer on that loop. Presents keep counting from the
       program's own threads while the main thread is blocked or gone, and a
       window then answers nothing; this is the one field that stops. Zero on
       a page from an engine that does not write it. */
    uint64_t main_beat_ns;
    /* When each of the newest frames was counted, in microseconds since start_ns, wrapping
       at 2^32 (71 minutes; the difference of two neighbors is still exact). The frames are
       the ones `frames`/`drawables` count for the rate: the presenter's when it presents,
       otherwise D3DMetal's drawables when it takes any, otherwise the driver's presents.
       Frame n is in ring[n % ring_capacity]; ring_head is the number written so far, and
       the newest slot may still be being written, so a reader stops one short of it. A page
       from an engine without the ring has ring_capacity 0. */
    uint64_t ring_head;
    uint32_t ring_capacity;
    uint32_t ring_reserved;
    uint32_t ring[SEVO_STATS_RING_CAPACITY];
};

/* Names the process for the page it will make. Called once, before any
   present; the page itself is made by the first present, so a process that
   never draws leaves no file. */
extern void sevo_stats_init(unsigned int appid, const char *exe);
extern unsigned int sevo_stats_appid(void);

extern void sevo_stats_note_present(unsigned int source);
extern void sevo_stats_note_drawable(void);
/* The main thread's run loop turned. Writes to a page a present has made and
   makes none, so a process that never draws still leaves no file. */
extern void sevo_stats_note_main_beat(void);
/* Frames so far (drawables when D3DMetal takes any, presents otherwise) and the
   Cocoa window number they go to; both 0 before the first present. */
extern unsigned long long sevo_stats_frame_count(unsigned long long *window_id);
/* The first window number a present path can name; later calls are ignored. */
extern void sevo_stats_note_window(unsigned long long window_id);
/* Copies the newest frame timestamps the ring holds, at most `max`, oldest first, into
   `out` (microseconds since the page was made, wrapping); returns how many. */
extern unsigned int sevo_stats_recent_frames(unsigned int *out, unsigned int max);

#endif  /* __WINE_SEVO_STATS_H */
