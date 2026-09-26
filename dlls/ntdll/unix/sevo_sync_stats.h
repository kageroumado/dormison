/*
 * Counters for the waiting paths, per thread, summed and written when the process exits.
 *
 * Off unless SEVO_SYNC_STATS names a file; each process appends one JSON line to it.
 * A counter costs one predictable branch when off and one unshared increment when on:
 * every thread counts into a block of its own, which outlives the thread so that the
 * threads a process killed are still in the sum.
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

#ifndef __SEVO_SYNC_STATS_H
#define __SEVO_SYNC_STATS_H

#define SEVO_SYNC_COUNTERS \
    X(alert_waits)              /* NtWaitForAlertByThreadId calls */ \
    X(alert_pending_on_entry)   /* the notification was already there */ \
    X(alert_poll_timeouts)      /* deadline already passed on entry */ \
    X(alert_spins_skipped)      /* waits the adaptive budget sent straight to the park */ \
    X(alert_spins)              /* spins started */ \
    X(alert_spin_hits)          /* spins that saw the notification */ \
    X(alert_arm_lost)           /* notified between the spin and the arm */ \
    X(alert_parks)              /* futex_wait calls */ \
    X(alert_park_woken)         /* ... that returned without an error */ \
    X(alert_park_timeouts)      /* ... that returned ETIMEDOUT */ \
    X(alert_park_errors)        /* ... that returned anything else */ \
    X(alert_timeouts)           /* waits that ended STATUS_TIMEOUT */ \
    X(alert_notifies)           /* NtAlertThreadByThreadId calls */ \
    X(alert_wake_calls)         /* ... that entered the kernel */ \
    X(alert_wake_nobody)        /* ... and found nobody (ENOENT) */ \
    X(yields)                   /* NtYieldExecution calls: SwitchToThread, Sleep(0) */ \
    X(sleeps_short)             /* NtDelayExecution of at most a millisecond, not zero */ \
    X(sleeps_long) \
    X(msync_waits)              /* msync_wait_objs calls */ \
    X(msync_direct_spins)       /* looks at an object's word before a park */ \
    X(msync_direct_spin_hits)   /* ... that saw the word move */ \
    X(msync_direct_parks)       /* sleeps on an object's own word */ \
    X(msync_registrations)      /* waits registered with the pump */ \
    X(msync_ack_turns)          /* turns of the acknowledgment spin, summed */ \
    X(msync_ack_parks)          /* acknowledgment waits that parked */ \
    X(msync_ack_contention)     /* registrations withdrawn because an object moved */ \
    X(msync_registered_parks)   /* sleeps on the thread's word after the acknowledgment */ \
    X(msync_removals)           /* removal messages sent */ \
    X(signal_all_calls)         /* kernel wake-all on an object's word */ \
    X(signal_all_messages)      /* ... followed by a message to the pump */ \
    X(signal_all_send_failures) \
    X(waitall_calls) \
    X(waitall_polls_refused)    /* polls answered here because a member was visibly missing */ \
    X(waitall_registrations)    /* sets sent to the pump */ \
    X(waitall_grants)           /* sets the pump granted and consumed */ \
    X(waitall_cancels)          /* registrations this side cancelled at the deadline */ \
    X(waitall_stale_wakes)      /* wakes that carried no verdict, registered again */ \
    X(waitall_commit_waits)     /* tokens found committing, waited for the grant */ \
    X(freeze_waits)             /* mutations that met a frozen object */ \
    X(freeze_parks)             /* ... and slept on its high word */

enum sevo_sync_counter
{
#define X(name) SEVO_STAT_##name,
    SEVO_SYNC_COUNTERS
#undef X
    SEVO_STAT_COUNT
};

#define SEVO_SPIN_HISTOGRAM_BUCKETS 24  /* bucket n counts spin hits at iteration [2^n, 2^(n+1)) */

struct sevo_sync_stats
{
    struct sevo_sync_stats *next;
    unsigned long long counters[SEVO_STAT_COUNT];
    unsigned long long spin_hit_iteration[SEVO_SPIN_HISTOGRAM_BUCKETS];
};

extern int sevo_sync_stats_enabled;
extern struct sevo_sync_stats *sevo_sync_stats_block(void);
extern void sevo_sync_stats_dump(void);

static inline void sevo_stat_add( enum sevo_sync_counter counter, unsigned long long amount )
{
    if (__builtin_expect( sevo_sync_stats_enabled, 0 ))
        sevo_sync_stats_block()->counters[counter] += amount;
}

#define SEVO_STAT(name) sevo_stat_add( SEVO_STAT_##name, 1 )
#define SEVO_STAT_ADD(name, amount) sevo_stat_add( SEVO_STAT_##name, (amount) )

static inline void sevo_stat_spin_hit( unsigned int iteration )
{
    if (__builtin_expect( sevo_sync_stats_enabled, 0 ))
    {
        unsigned int bucket = iteration ? 31 - __builtin_clz( iteration ) : 0;
        if (bucket >= SEVO_SPIN_HISTOGRAM_BUCKETS) bucket = SEVO_SPIN_HISTOGRAM_BUCKETS - 1;
        sevo_sync_stats_block()->spin_hit_iteration[bucket]++;
    }
}

#endif /* __SEVO_SYNC_STATS_H */
