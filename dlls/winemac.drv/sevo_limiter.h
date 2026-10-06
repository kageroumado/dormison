/*
 * The frame limiter: holds a program's presents to a frame rate.
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

#ifndef __WINE_SEVO_LIMITER_H
#define __WINE_SEVO_LIMITER_H

#include <stdint.h>

/* The limit in frames per second, 0 for none: `Mac Driver\FrameRateLimit=<n>` or
   SEVO_FPS_LIMIT=<n>, and View > Frame Rate Limit while the game runs. */
extern int frame_rate_limit;

/* One present path's schedule. Zeroed, it starts at the next frame. */
struct sevo_limiter
{
    uint64_t deadline;   /* mach ticks; when the next frame may leave */
    uint64_t interval;   /* mach ticks per frame at the limit the deadline was set for */
};

/* Called on the program's thread just before a frame leaves for the screen: sleeps until
   the frame's deadline. Deadlines advance by one interval from the previous deadline, not
   from when the frame arrived, so the rate holds without drift; a frame later than a whole
   interval starts the schedule again rather than letting the next ones through in a burst. */
extern void sevo_limiter_wait(struct sevo_limiter *limiter);

/* Sets the limit for every path in the process at once. */
extern void sevo_limiter_set(int fps);

#endif  /* __WINE_SEVO_LIMITER_H */
