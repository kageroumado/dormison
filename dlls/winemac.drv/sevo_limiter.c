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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <mach/mach_time.h>
#include <pthread.h>

#include "sevo_limiter.h"

int frame_rate_limit = 0;

static mach_timebase_info_data_t timebase;
static pthread_once_t timebase_once = PTHREAD_ONCE_INIT;

static void read_timebase(void)
{
    mach_timebase_info(&timebase);
    if (!timebase.numer || !timebase.denom) timebase.numer = timebase.denom = 1;
}

/* One frame at `fps`, in the unit mach_absolute_time and mach_wait_until share. */
static uint64_t frame_ticks(int fps)
{
    pthread_once(&timebase_once, read_timebase);
    return (uint64_t)(1e9 / fps * timebase.denom / timebase.numer + 0.5);
}

void sevo_limiter_set(int fps)
{
    __atomic_store_n(&frame_rate_limit, fps > 0 ? fps : 0, __ATOMIC_RELAXED);
}

void sevo_limiter_wait(struct sevo_limiter *limiter)
{
    int fps = __atomic_load_n(&frame_rate_limit, __ATOMIC_RELAXED);
    uint64_t interval, deadline, now;

    if (fps <= 0)
    {
        limiter->deadline = 0;
        return;
    }

    interval = frame_ticks(fps);
    deadline = limiter->deadline;
    now = mach_absolute_time();
    if (!deadline || limiter->interval != interval || now > deadline + interval)
        deadline = now;
    else if (now < deadline)
        mach_wait_until(deadline);

    limiter->deadline = deadline + interval;
    limiter->interval = interval;
}
