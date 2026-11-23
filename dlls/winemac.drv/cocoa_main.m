/*
 * MACDRV Cocoa initialization code
 *
 * Copyright 2011, 2012, 2013 Ken Thomases for CodeWeavers Inc.
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

#import <AppKit/AppKit.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdatomic.h>

#include "macdrv_cocoa.h"
#import "cocoa_app.h"
#include "sevo_stats.h"

#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"


/* Condition values for an NSConditionLock. Used to signal between run_cocoa_app
   and macdrv_start_cocoa_app so the latter knows when the former is running
   the application event loop. */
enum {
    COCOA_APP_NOT_RUNNING,
    COCOA_APP_RUNNING,
};


/* How long macdrv_start_cocoa_app waits for the main thread. A process that
   gives up runs without a display driver for the rest of its life, and on a
   loaded Mac the main thread of a just-started process (Steam's overlay
   beside a game that is loading) can be busy for much longer than a few
   seconds. */
static const NSTimeInterval cocoa_app_start_timeout = 60;


/* Shared by macdrv_start_cocoa_app, which waits, and run_cocoa_app, which the
   main thread reaches whenever its run loop next turns: possibly after the
   waiter has given up and returned. Each of the two holds one of `owners`,
   and the last to let go frees the lock and the block. */
struct cocoa_app_startup_info {
    NSConditionLock*    lock;
    unsigned long long  tickcount;
    uint64_t            uptime_ns;
    BOOL                success;
    atomic_int          owners;
};

static void release_startup_info(struct cocoa_app_startup_info* startup_info)
{
    if (atomic_fetch_sub(&startup_info->owners, 1) == 1)
    {
        [startup_info->lock release];
        free(startup_info);
    }
}


/***********************************************************************
 *              run_cocoa_app
 *
 * Transforms the main thread from merely idling in its run loop to
 * being a Cocoa application running its event loop.
 *
 * This will be the perform callback of a custom run loop source that
 * will be scheduled in the main thread's run loop from a secondary
 * thread by macdrv_start_cocoa_app.  This function communicates that
 * it has successfully started the application by changing the condition
 * of a shared NSConditionLock, passed in via the info parameter.
 *
 * This function never returns.  It's the new permanent home of the
 * main thread.
 */
static void run_cocoa_app(void* info)
{
    struct cocoa_app_startup_info* startup_info = info;
    NSConditionLock* lock = startup_info->lock;
    BOOL created_app = FALSE;
    BOOL success;

    @autoreleasepool
    {
        if (!NSApp)
        {
            [WineApplication sharedApplication];
            created_app = TRUE;
        }

        if ([NSApp respondsToSelector:@selector(setWineController:)])
        {
            WineApplicationController* controller = [WineApplicationController sharedController];
            [NSApp setWineController:controller];
            [controller computeEventTimeAdjustmentFromTicks:startup_info->tickcount uptime:startup_info->uptime_ns];
            startup_info->success = TRUE;
        }

        success = startup_info->success;
        [lock lock];
        [lock unlockWithCondition:COCOA_APP_RUNNING];
        release_startup_info(startup_info);
    }

    if (created_app && success)
    {
        @autoreleasepool
        {
            /* Once a second, in every common mode, so a menu or a window drag
               does not read as a stopped main thread. */
            NSTimer* beat = [NSTimer timerWithTimeInterval:1 repeats:YES block:^(NSTimer* timer){
                sevo_stats_note_main_beat();
            }];
            beat.tolerance = 0.2;
            [[NSRunLoop mainRunLoop] addTimer:beat forMode:NSRunLoopCommonModes];

            /* Never returns */
            [NSApp run];
        }
    }
}


/***********************************************************************
 *              macdrv_start_cocoa_app
 *
 * Tells the main thread to transform itself into a Cocoa application.
 *
 * Returns 0 on success, non-zero on failure.
 */
int macdrv_start_cocoa_app(unsigned long long tickcount)
{
@autoreleasepool
{
    int ret = -1;
    CFRunLoopSourceRef source;
    struct cocoa_app_startup_info* startup_info;
    uint64_t uptime_mach = mach_absolute_time();
    mach_timebase_info_data_t mach_timebase;
    NSDate* timeLimit;
    CFRunLoopSourceContext source_context = { 0 };

    /* Make sure Cocoa is in multi-threading mode by detaching a
       do-nothing thread. */
    [NSThread detachNewThreadSelector:@selector(self)
                             toTarget:[NSThread class]
                           withObject:nil];

    if (!(startup_info = calloc(1, sizeof(*startup_info)))) return ret;
    startup_info->lock = [[NSConditionLock alloc] initWithCondition:COCOA_APP_NOT_RUNNING];
    startup_info->tickcount = tickcount;
    atomic_init(&startup_info->owners, 1);

    mach_timebase_info(&mach_timebase);
    startup_info->uptime_ns = uptime_mach * mach_timebase.numer / mach_timebase.denom;

    timeLimit = [NSDate dateWithTimeIntervalSinceNow:cocoa_app_start_timeout];

    source_context.info = startup_info;
    source_context.perform = run_cocoa_app;
    source = CFRunLoopSourceCreate(NULL, 0, &source_context);

    if (source && startup_info->lock && timeLimit)
    {
        /* run_cocoa_app's share, taken before the main thread can reach it. */
        atomic_fetch_add(&startup_info->owners, 1);
        CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
        CFRunLoopSourceSignal(source);
        CFRunLoopWakeUp(CFRunLoopGetMain());

        if ([startup_info->lock lockWhenCondition:COCOA_APP_RUNNING beforeDate:timeLimit])
        {
            ret = !startup_info->success;
            [startup_info->lock unlock];
        }
    }

    if (source)
        CFRelease(source);
    release_startup_info(startup_info);
    return ret;
}
}
