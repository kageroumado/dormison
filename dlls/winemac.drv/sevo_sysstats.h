/*
 * The overlay's system row: this process's CPU, the GPU's load, the Mac's power and temperature.
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

#ifndef __WINE_SEVO_SYSSTATS_H
#define __WINE_SEVO_SYSSTATS_H

/* One reading. A value the Mac does not offer is negative. */
struct sevo_system_stats
{
    /* This process's CPU time over the wall time since the previous reading, in per cent of
       one core, the way Activity Monitor counts it: a game busy on three cores reads 300. */
    float cpu_percent;
    /* The GPU's "Device Utilization %" from IOAccelerator's PerformanceStatistics: the
       whole GPU, every process on it. */
    float gpu_percent;
    /* The whole Mac's power draw in watts (SMC `PSTR`). */
    float power_watts;
    /* The CPU's temperature in °C: the mean of the core sensors the SMC offers. */
    float cpu_celsius;
};

/* Takes a reading. It costs an IOKit registry read and a few SMC calls, plus a one-time walk
   of the SMC's key list on the first call, so it runs off the main thread, on one thread at a
   time. */
extern void sevo_system_stats_sample(struct sevo_system_stats *stats);

#endif  /* __WINE_SEVO_SYSSTATS_H */
