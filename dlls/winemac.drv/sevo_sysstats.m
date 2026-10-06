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

#import <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "sevo_sysstats.h"

/* The SMC's call structure, `SMCParamStruct` in Apple's AppleSMC user client: 80 bytes, the
   kernel refuses any other size. */
struct smc_param
{
    uint32_t key;
    uint8_t  vers[6];
    uint16_t vers_pad;
    uint16_t plimit_version, plimit_length;
    uint32_t plimit_cpu, plimit_gpu, plimit_mem;
    uint32_t data_size;
    uint32_t data_type;
    uint8_t  data_attributes, pad[3];
    uint8_t  result, status, command, pad2;
    uint32_t data32;
    uint8_t  bytes[32];
};

_Static_assert(sizeof(struct smc_param) == 80, "AppleSMC takes exactly 80 bytes");

enum
{
    SMC_SELECTOR      = 2,
    SMC_READ_KEY      = 5,
    SMC_KEY_AT_INDEX  = 8,
    SMC_KEY_INFO      = 9,
};

#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* Core sensors found at most (an M-series Max has about forty), and how many of them, spread
   evenly over the list, a reading averages: each costs an SMC call of about 150 µs. */
#define MAX_CORE_SENSORS 64
#define READ_CORE_SENSORS 8

/* A key worth reading again, with the type and size its info call returned, so a reading
   is one SMC call rather than two. */
struct smc_key
{
    uint32_t key;
    uint32_t type;
    uint32_t size;
};

static io_connect_t smc;
static BOOL smc_tried;
static struct smc_key core_sensors[MAX_CORE_SENSORS];
static unsigned int core_sensor_count;
static struct smc_key power_key;
static io_registry_entry_t accelerator;

static BOOL smc_call(struct smc_param *input, struct smc_param *output)
{
    size_t size = sizeof(*output);

    memset(output, 0, sizeof(*output));
    return IOConnectCallStructMethod(smc, SMC_SELECTOR, input, sizeof(*input), output, &size) == kIOReturnSuccess
           && !output->result;
}

static BOOL smc_key_info(uint32_t key, struct smc_key *info)
{
    struct smc_param input = { .key = key, .command = SMC_KEY_INFO }, output;

    if (!smc_call(&input, &output)) return FALSE;
    info->key = key;
    info->type = output.data_type;
    info->size = output.data_size;
    return TRUE;
}

/* A key's value as a float, from the two formats temperature and power keys use. */
static BOOL smc_read_float(const struct smc_key *info, float *value)
{
    struct smc_param input = { .key = info->key, .command = SMC_READ_KEY, .data_size = info->size }, output;

    if (!smc_call(&input, &output)) return FALSE;
    if (info->type == FOURCC('f','l','t',' ') && info->size == 4)
    {
        memcpy(value, output.bytes, sizeof(*value));
        return TRUE;
    }
    if (info->type == FOURCC('s','p','7','8') && info->size == 2)
    {
        *value = (int16_t)((output.bytes[0] << 8) | output.bytes[1]) / 256.0f;
        return TRUE;
    }
    return FALSE;
}

static BOOL plausible_celsius(float value)
{
    return value > 5 && value < 120;
}

/* Opens the SMC and finds its CPU core sensors once: on Apple silicon every `Tp…` and `Te…`
   key that reads a plausible temperature, on an Intel Mac the classic proximity sensor. */
static void smc_open(void)
{
    io_service_t service;
    struct smc_param input = { .key = FOURCC('#','K','E','Y'), .command = SMC_KEY_INFO }, output;
    struct smc_key info;
    uint32_t count, i;
    float value;

    smc_tried = TRUE;
    service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (!service) return;
    if (IOServiceOpen(service, mach_task_self(), 0, &smc) != kIOReturnSuccess) smc = 0;
    IOObjectRelease(service);
    if (!smc) return;

    if (smc_call(&input, &output))
    {
        input.command = SMC_READ_KEY;
        input.data_size = output.data_size;
        if (smc_call(&input, &output))
        {
            count = ((uint32_t)output.bytes[0] << 24) | ((uint32_t)output.bytes[1] << 16) |
                    ((uint32_t)output.bytes[2] << 8) | output.bytes[3];
            for (i = 0; i < count && core_sensor_count < MAX_CORE_SENSORS; i++)
            {
                struct smc_param at = { .command = SMC_KEY_AT_INDEX, .data32 = i };
                uint32_t prefix;

                if (!smc_call(&at, &output)) continue;
                prefix = output.key >> 16;
                if (prefix != FOURCC(0,0,'T','p') && prefix != FOURCC(0,0,'T','e')) continue;
                if (smc_key_info(output.key, &info) && smc_read_float(&info, &value) && plausible_celsius(value))
                    core_sensors[core_sensor_count++] = info;
            }
        }
    }
    if (!core_sensor_count && smc_key_info(FOURCC('T','C','0','P'), &info) &&
        smc_read_float(&info, &value) && plausible_celsius(value))
        core_sensors[core_sensor_count++] = info;

    if (core_sensor_count > READ_CORE_SENSORS)
    {
        for (i = 0; i < READ_CORE_SENSORS; i++)
            core_sensors[i] = core_sensors[i * core_sensor_count / READ_CORE_SENSORS];
        core_sensor_count = READ_CORE_SENSORS;
    }

    if (!smc_key_info(FOURCC('P','S','T','R'), &power_key) || !smc_read_float(&power_key, &value))
        power_key.key = 0;
}

static float cpu_celsius(void)
{
    float sum = 0, value;
    unsigned int i, n = 0;

    for (i = 0; i < core_sensor_count; i++)
    {
        if (smc_read_float(&core_sensors[i], &value) && plausible_celsius(value))
        {
            sum += value;
            n++;
        }
    }
    return n ? sum / n : -1;
}

static float power_watts(void)
{
    float value;

    if (!power_key.key || !smc_read_float(&power_key, &value) || value < 0) return -1;
    return value;
}

/* The first accelerator's utilization; an Apple silicon Mac has one. */
static float gpu_percent(void)
{
    CFDictionaryRef statistics;
    float percent = -1;

    if (!accelerator)
        accelerator = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAccelerator"));
    if (!accelerator) return -1;

    statistics = IORegistryEntryCreateCFProperty(accelerator, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
    if (statistics && CFGetTypeID(statistics) == CFDictionaryGetTypeID())
    {
        CFNumberRef number = CFDictionaryGetValue(statistics, CFSTR("Device Utilization %"));
        int value;
        if (number && CFGetTypeID(number) == CFNumberGetTypeID() &&
            CFNumberGetValue(number, kCFNumberIntType, &value))
            percent = value;
    }
    if (statistics) CFRelease(statistics);
    return percent;
}

/* getrusage's times are already microseconds: the kernel converts its own ticks, which a
   Rosetta process could not do with the timebase it is shown. */
static float cpu_percent(void)
{
    static uint64_t last_cpu_us, last_wall_ns;
    struct rusage usage;
    uint64_t cpu_us, wall_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    float percent = -1;

    if (getrusage(RUSAGE_SELF, &usage)) return -1;
    cpu_us = (uint64_t)usage.ru_utime.tv_sec * 1000000 + usage.ru_utime.tv_usec +
             (uint64_t)usage.ru_stime.tv_sec * 1000000 + usage.ru_stime.tv_usec;
    if (last_wall_ns && wall_ns > last_wall_ns)
        percent = (cpu_us - last_cpu_us) * 1000.0f / (wall_ns - last_wall_ns) * 100;
    last_cpu_us = cpu_us;
    last_wall_ns = wall_ns;
    return percent;
}

void sevo_system_stats_sample(struct sevo_system_stats *stats)
{
    @autoreleasepool
    {
        if (!smc_tried) smc_open();
        stats->cpu_percent = cpu_percent();
        stats->gpu_percent = gpu_percent();
        stats->power_watts = smc ? power_watts() : -1;
        stats->cpu_celsius = smc ? cpu_celsius() : -1;
    }
}
