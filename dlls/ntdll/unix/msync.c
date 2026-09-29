/*
 * mach semaphore-based synchronization objects
 *
 * Copyright (C) 2018 Zebediah Figura
 * Copyright (C) 2023 Marc-Aurel Zent
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

#ifdef __APPLE__

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/notify.h>
#include <mach/mach_vm.h>
#include <mach/vm_page_size.h>
#include <mach/message.h>
#include <mach/port.h>
#include <mach/task.h>
#include <mach/semaphore.h>
#include <mach/mach_error.h>
#include <servers/bootstrap.h>
#include <os/lock.h>
#include <AvailabilityMacros.h>
#include <dlfcn.h>
#include <sched.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wine/server.h"

#include "unix_private.h"
#include "msync.h"
#include "sevo_sync_stats.h"

WINE_DEFAULT_DEBUG_CHANNEL(sync);

static LONGLONG update_timeout( ULONGLONG end )
{
    LARGE_INTEGER now;
    LONGLONG timeleft;

    NtQuerySystemTime( &now );
    timeleft = end - now.QuadPart;
    if (timeleft < 0) timeleft = 0;
    return timeleft;
}

#define UL_COMPARE_AND_WAIT_SHARED  0x3
#define ULF_WAKE_ALL                0x00000100
#define ULF_NO_ERRNO                0x01000000
extern int __ulock_wake( uint32_t operation, void *addr, uint64_t wake_value );
extern int __ulock_wait( uint32_t operation, void *addr, uint64_t value, uint32_t timeout ); /* timeout is specified in microseconds */
#ifdef MAC_OS_VERSION_11_0
extern int __ulock_wait2( uint32_t operation, void *addr, uint64_t value, uint64_t timeout_ns, uint64_t value2 ) __attribute__((weak_import));
#endif

static inline int ulock_wait( uint32_t operation, void *addr, uint64_t value, uint64_t timeout_ns )
{
#ifdef MAC_OS_VERSION_11_0
    if (__builtin_available( macOS 11.0, * ))
    {
        return __ulock_wait2( operation, addr, value, timeout_ns, 0 );
    }
    else
#endif
    {
        uint32_t timeout_us = timeout_ns / 1000;
        /* Avoid a 0 timeout for small timeout_ns values */
        uint32_t adjust = (timeout_us == 0) & (timeout_ns != 0);
        return __ulock_wait( operation, addr, value, timeout_us + adjust );
    }
}

/*
 * Faster to directly do the syscall and inline everything, taken and slightly adapted
 * from xnu/libsyscall/mach/mach_msg.c
 */

#define LIBMACH_OPTIONS64 (MACH_SEND_INTERRUPT|MACH_RCV_INTERRUPT)
#define MACH64_SEND_MQ_CALL 0x0000000400000000ull

typedef mach_msg_return_t (*mach_msg2_trap_ptr_t)( void *data, uint64_t options,
    uint64_t msgh_bits_and_send_size, uint64_t msgh_remote_and_local_port,
    uint64_t msgh_voucher_and_id, uint64_t desc_count_and_rcv_name,
    uint64_t rcv_size_and_priority, uint64_t timeout );

static mach_msg2_trap_ptr_t mach_msg2_trap;

static inline mach_msg_return_t mach_msg2_internal( void *data, uint64_t option64, uint64_t msgh_bits_and_send_size,
    uint64_t msgh_remote_and_local_port, uint64_t msgh_voucher_and_id, uint64_t desc_count_and_rcv_name,
    uint64_t rcv_size_and_priority, uint64_t timeout)
{
    mach_msg_return_t mr;

    mr = mach_msg2_trap( data, option64 & ~LIBMACH_OPTIONS64, msgh_bits_and_send_size,
             msgh_remote_and_local_port, msgh_voucher_and_id, desc_count_and_rcv_name,
             rcv_size_and_priority, timeout );

    if (mr == MACH_MSG_SUCCESS)
        return MACH_MSG_SUCCESS;

    while (mr == MACH_SEND_INTERRUPTED)
        mr = mach_msg2_trap( data, option64 & ~LIBMACH_OPTIONS64, msgh_bits_and_send_size,
                 msgh_remote_and_local_port, msgh_voucher_and_id, desc_count_and_rcv_name,
                 rcv_size_and_priority, timeout );

    while (mr == MACH_RCV_INTERRUPTED)
        mr = mach_msg2_trap( data, option64 & ~LIBMACH_OPTIONS64, msgh_bits_and_send_size & 0xffffffffull,
                 msgh_remote_and_local_port, msgh_voucher_and_id, desc_count_and_rcv_name,
                 rcv_size_and_priority, timeout);

    return mr;
}

static inline mach_msg_return_t mach_msg2( mach_msg_header_t *data, uint64_t option64,
    mach_msg_size_t send_size, mach_msg_size_t rcv_size, mach_port_t rcv_name, uint64_t timeout,
    uint32_t priority)
{
    mach_msg_base_t *base;
    mach_msg_size_t descriptors;

    if (!mach_msg2_trap)
        return mach_msg( data, (mach_msg_option_t)option64, send_size,
                         rcv_size, rcv_name, timeout, priority );

    base = (mach_msg_base_t *)data;

    if ((option64 & MACH_SEND_MSG) &&
        (base->header.msgh_bits & MACH_MSGH_BITS_COMPLEX))
        descriptors = base->body.msgh_descriptor_count;
    else
        descriptors = 0;

#define MACH_MSG2_SHIFT_ARGS(lo, hi) ((uint64_t)hi << 32 | (uint32_t)lo)
    return mach_msg2_internal(data, option64 | MACH64_SEND_MQ_CALL,
               MACH_MSG2_SHIFT_ARGS(data->msgh_bits, send_size),
               MACH_MSG2_SHIFT_ARGS(data->msgh_remote_port, data->msgh_local_port),
               MACH_MSG2_SHIFT_ARGS(data->msgh_voucher_port, data->msgh_id),
               MACH_MSG2_SHIFT_ARGS(descriptors, rcv_name),
               MACH_MSG2_SHIFT_ARGS(rcv_size, priority), timeout);
#undef MACH_MSG2_SHIFT_ARGS
}

/* Every object is 16 bytes: one 64-bit state word, then the type, the shared reference
 * count and the waiter-interest count. The low half of the word is the semaphore count,
 * the event's signaled flag or the mutex owner; the high half is the semaphore maximum,
 * zero for an event, or the mutex recursion depth. Bit 31 of the high half is MSYNC_FROZEN:
 * the wineserver's pump sets it on every member of a WaitAll set while it evaluates and
 * consumes the set, and nothing else ever holds it. Every mutation here is one 64-bit
 * compare-and-swap whose expected value has the bit clear, so a frozen object cannot move
 * under the pump, and a mutation that meets the bit sleeps until the thaw. A client never
 * sets the bit, so a client that dies leaves nothing frozen. */
struct semaphore
{
    int count;
    int max;
    unsigned short msync_type;
    unsigned short refcount;
    int multiple_waiters;
};
C_ASSERT(sizeof(struct semaphore) == 16);

struct event
{
    int signaled;
    unsigned int high;
    unsigned short msync_type;
    unsigned short refcount;
    int multiple_waiters;
};
C_ASSERT(sizeof(struct event) == 16);
C_ASSERT(FIELD_OFFSET(struct event, high) == sizeof(int));

struct mutex
{
    int tid;
    int count;  /* recursion count */
    unsigned short msync_type;
    unsigned short refcount;
    int multiple_waiters;
};
C_ASSERT(sizeof(struct mutex) == 16);

#define MSYNC_FROZEN 0x80000000u
/* Windows raises STATUS_MUTANT_LIMIT_EXCEEDED two levels later; the bit above this is MSYNC_FROZEN. */
#define MSYNC_MUTEX_RECURSION_MAX 0x7fffffff

static inline uint64_t load_object( const void *obj )
{
    return __atomic_load_n( (const uint64_t *)obj, __ATOMIC_SEQ_CST );
}

static inline int object_low( uint64_t word )
{
    return (int)(unsigned int)word;
}

static inline unsigned int object_high( uint64_t word )
{
    return (unsigned int)(word >> 32) & ~MSYNC_FROZEN;
}

static inline BOOL object_frozen( uint64_t word )
{
    return ((unsigned int)(word >> 32) & MSYNC_FROZEN) != 0;
}

static inline uint64_t make_object( int low, unsigned int high )
{
    return ((uint64_t)high << 32) | (unsigned int)low;
}

/* On failure *expected holds the word found, as with __atomic_compare_exchange_n. */
static inline BOOL swap_object( void *obj, uint64_t *expected, uint64_t desired )
{
    return __atomic_compare_exchange_n( (uint64_t *)obj, expected, desired, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST );
}

/* Every object of the set, the alert object, and for a WaitAll the registration's
 * generation after them. */
typedef struct
{
    mach_msg_header_t header;
    unsigned int shm_idx[MAXIMUM_WAIT_OBJECTS + 2];
} mach_register_message_t;

/* Bits above the 28-bit object index in a registration message's entries; server/msync.c
 * reads the same ones. */
#define REGISTER_MUTEX    (1u << 28)  /* on any entry: the object is a mutex */
#define REGISTER_REMOVE   (1u << 29)  /* on the first entry: the message removes the wait */
#define REGISTER_WAIT_ALL (1u << 30)  /* on the first entry: the set is a WaitAll */
#define REGISTER_ALERT    (1u << 31)  /* on an entry of a WaitAll: the thread's alert object */

static mach_port_t server_port;

static int *shm_tid_map;

static const mach_msg_bits_t msgh_bits_send = MACH_MSGH_BITS_REMOTE(MACH_MSG_TYPE_COPY_SEND);

static inline void drop_waiter_interest( struct event *obj )
{
    int refs = __atomic_sub_fetch( &obj->multiple_waiters, 1, __ATOMIC_SEQ_CST);
    if (refs < 0)
        __atomic_store_n( &obj->multiple_waiters, 0, __ATOMIC_SEQ_CST);
}

/* Every object of a registration, the alert object included, holds one unit
 * of interest from the increment in server_register_wait until this drop. */
static inline void drop_wait_interest( void **objs_shm, void *alert_obj_shm, int count )
{
    int i;

    for (i = 0; i < count; i++)
        drop_waiter_interest( (struct event *)objs_shm[i] );

    if (alert_obj_shm)
        drop_waiter_interest( (struct event *)alert_obj_shm );
}

/* MACH_SEND_INVALID_DEST from a send to server_port means the server's receive
 * right is gone: the wineserver process died. The port was looked up once in
 * msync_init, so no replacement server is reachable from this process, and a
 * caller that retried would spin on the same error for as long as it lived.
 * The thread ends the way the socket path ends it when the server closes the
 * connection (send_request on EPIPE); the last thread takes the process down. */
static void abort_if_server_gone( mach_msg_return_t mr )
{
    if (mr != MACH_SEND_INVALID_DEST) return;
    ERR( "msync server is gone (the wineserver died); this thread ends\n" );
    abort_thread( 0 );
}

static inline mach_msg_return_t server_register_wait( unsigned int tid, const int *objs,
                                void **objs_shm, int alert_obj, void *alert_obj_shm, int count,
                                unsigned int generation )
{
    int i, is_mutex, total = count;
    BOOL wait_all = generation != 0;
    mach_msg_return_t mr;
    __thread static mach_register_message_t message;

    message.header.msgh_remote_port = server_port;
    message.header.msgh_bits = msgh_bits_send;

    for (i = 0; i < count; i++)
    {
        struct event *obj = (struct event *)objs_shm[i];

        is_mutex = obj->msync_type == MSYNC_MUTEX ? 1 : 0;
        message.shm_idx[i] = objs[i] | (is_mutex ? REGISTER_MUTEX : 0);
        __atomic_add_fetch( &obj->multiple_waiters, 1, __ATOMIC_SEQ_CST);
    }

    if (alert_obj)
    {
        struct event *obj = (struct event *)alert_obj_shm;

        message.shm_idx[total++] = alert_obj | (wait_all ? REGISTER_ALERT : 0);
        __atomic_add_fetch( &obj->multiple_waiters, 1, __ATOMIC_SEQ_CST);
    }

    if (wait_all)
    {
        message.shm_idx[0] |= REGISTER_WAIT_ALL;
        message.shm_idx[total++] = generation;
    }

    message.header.msgh_id = (tid << 8) | total;
    message.header.msgh_size = sizeof(mach_msg_header_t) +
                               total * sizeof(unsigned int);

    mr = mach_msg2( (mach_msg_header_t *)&message, MACH_SEND_MSG, message.header.msgh_size,
                     0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, 0 );

    if (mr != MACH_MSG_SUCCESS)
    {
        abort_if_server_gone( mr );
        ERR("Failed to send server register wait: %#x\n", mr);
        /* The server never saw this registration, so there is nothing to remove. */
        drop_wait_interest( objs_shm, alert_obj ? alert_obj_shm : NULL, count );
    }

    return mr;
}

static inline void server_remove_wait( unsigned int msgh_id, const int *objs, void **objs_shm,
                                       int alert_obj, void *alert_obj_shm, int count )
{
    int i;
    mach_msg_return_t mr;
    __thread static mach_register_message_t message;

    message.header.msgh_remote_port = server_port;
    message.header.msgh_bits = msgh_bits_send;
    message.header.msgh_id = msgh_id;

    drop_wait_interest( objs_shm, alert_obj ? alert_obj_shm : NULL, count );

    for (i = 0; i < count; i++)
        message.shm_idx[i] = objs[i];

    if (alert_obj)
        message.shm_idx[count++] = alert_obj;

    message.shm_idx[0] |= REGISTER_REMOVE;
    SEVO_STAT( msync_removals );

    message.header.msgh_size = sizeof(mach_msg_header_t) +
                               count * sizeof(unsigned int);

    mr = mach_msg2( (mach_msg_header_t *)&message, MACH_SEND_MSG, message.header.msgh_size,
                     0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, 0 );

    if (mr != MACH_MSG_SUCCESS)
    {
        abort_if_server_gone( mr );
        ERR("Failed to send server remove wait: %#x\n", mr);
    }
}

#if defined(__x86_64__) || defined(__i386__)
#define YIELD_PROCESSOR __asm__ __volatile__( "pause" ::: "memory" )
#elif defined(__aarch64__)
#define YIELD_PROCESSOR __asm__ __volatile__( "yield" ::: "memory" )
#else
#define YIELD_PROCESSOR do {} while (0)
#endif

#define SEVO_PARK_SLICE_MS_MAX 60000

/* How long one park lasts before the waiter looks at its word again, in nanoseconds;
 * 0, the default, parks for as long as the caller asked. SEVO_PARK_SLICE_MS sets it. Every
 * slice is a wakeup of a parked thread, so it stays off unless a wedge is being chased. */
static uint64_t park_slice_ns(void)
{
    /* Published atomically: every thread that races here computes the same value. */
    static int slice_ms = -1;
    int value = __atomic_load_n( &slice_ms, __ATOMIC_RELAXED );

    if (value < 0)
    {
        value = sevo_env_budget( "SEVO_PARK_SLICE_MS", 0, SEVO_PARK_SLICE_MS_MAX );
        __atomic_store_n( &slice_ms, value, __ATOMIC_RELAXED );
    }
    return (uint64_t)value * 1000000;
}

/* Parks on a shared word while it holds `expected`, for at most `timeout_ns` (0: no
 * limit), in slices of park_slice_ns(). A word found changed between slices ends the park
 * as if woken. Returns what ulock_wait returns: 0 on a wake, -ETIMEDOUT only once
 * `timeout_ns` has passed. */
static int ulock_park( void *addr, int expected, uint64_t timeout_ns )
{
    uint64_t slice = park_slice_ns(), start = 0;
    int ret;

    if (!slice) return ulock_wait( UL_COMPARE_AND_WAIT_SHARED | ULF_NO_ERRNO, addr, expected, timeout_ns );
    if (timeout_ns) start = clock_gettime_nsec_np( CLOCK_UPTIME_RAW );
    for (;;)
    {
        uint64_t wait = slice;

        if (timeout_ns)
        {
            uint64_t spent = clock_gettime_nsec_np( CLOCK_UPTIME_RAW ) - start;
            if (spent >= timeout_ns) return -ETIMEDOUT;
            if (timeout_ns - spent < wait) wait = timeout_ns - spent;
        }
        ret = ulock_wait( UL_COMPARE_AND_WAIT_SHARED | ULF_NO_ERRNO, addr, expected, wait );
        if (ret != -ETIMEDOUT) return ret;
        if (timeout_ns && clock_gettime_nsec_np( CLOCK_UPTIME_RAW ) - start >= timeout_ns) return -ETIMEDOUT;
        if (__atomic_load_n( (const int *)addr, __ATOMIC_ACQUIRE ) != expected) return 0;
    }
}

/* Looks at a frozen object's high word this many times before parking on it. The pump's
 * freeze lasts single-digit microseconds unless the pump is preempted. */
#define FREEZE_SPIN 8192

/* Sleeps until the pump thaws the object; the caller reloads the word afterwards. The
 * pump clears MSYNC_FROZEN and wakes the high word on every thaw, so a compare-and-wait
 * against the frozen value returns at once when the thaw came first. */
static void wait_for_thaw( void *obj, uint64_t seen )
{
    unsigned int *high = (unsigned int *)obj + 1, frozen = (unsigned int)(seen >> 32);
    int i, ret;

    SEVO_STAT( freeze_waits );
    for (i = 0; i < FREEZE_SPIN; i++)
    {
        YIELD_PROCESSOR;
        if (__atomic_load_n( high, __ATOMIC_ACQUIRE ) != frozen) return;
    }
    SEVO_STAT( freeze_parks );
    do ret = ulock_park( high, (int)frozen, 0 );
    while (ret == -EINTR || ret == -EFAULT);
}

/* The object's word once it is not frozen. */
static inline uint64_t thawed_object( void *obj, uint64_t word )
{
    while (object_frozen( word ))
    {
        wait_for_thaw( obj, word );
        word = load_object( obj );
    }
    return word;
}

#define SEVO_OBJECT_SPIN_MAX 1000000
#define OBJECT_SPIN_BATCH 512

/* How long a wait on one object looks at the object's word before parking on it, in
 * YIELD_PROCESSOR iterations. SEVO_OBJECT_SPIN sets it; 0 parks on the first look. */
static int object_spin_budget(void)
{
    /* Published atomically: every thread that races here computes the same value. */
    static int budget = -1;
    int value = __atomic_load_n( &budget, __ATOMIC_RELAXED );

    if (value < 0)
    {
        value = sevo_env_budget( "SEVO_OBJECT_SPIN", 0, SEVO_OBJECT_SPIN_MAX );
        __atomic_store_n( &budget, value, __ATOMIC_RELAXED );
    }
    return value;
}

/* Reads only. TRUE when the word moved off val, so the caller's next grab is worth making.
 * The spin is part of the wait and ends at the wait's deadline, looked at once a batch. */
static inline BOOL spin_for_object( const int *word, int val, const ULONGLONG *end )
{
    int budget = object_spin_budget();

    if (budget > 0) SEVO_STAT( msync_direct_spins );
    while (budget > 0)
    {
        int batch = budget < OBJECT_SPIN_BATCH ? budget : OBJECT_SPIN_BATCH;

        budget -= batch;
        while (batch--)
        {
            YIELD_PROCESSOR;
            if (__atomic_load_n( word, __ATOMIC_RELAXED ) != val)
            {
                SEVO_STAT( msync_direct_spin_hits );
                return TRUE;
            }
        }
        if (end && !update_timeout( *end )) return FALSE;
    }
    return FALSE;
}

static inline NTSTATUS msync_wait_single( int obj, void *obj_shm,
                                          ULONGLONG *end, int tid )
{
    int ret, val = 0;
    ULONGLONG ns_timeleft = 0;

    do
    {
        if (((struct mutex *)obj_shm)->msync_type == MSYNC_MUTEX)
        {
            val = __atomic_load_n( (int *)obj_shm, __ATOMIC_ACQUIRE );
            if (!val || val == ~0)
                val = tid;
        }

        if (__atomic_load_n( (int *)obj_shm, __ATOMIC_ACQUIRE ) != val)
            return STATUS_PENDING;

        if (spin_for_object( (const int *)obj_shm, val, end ))
            return STATUS_PENDING;

        if (end)
        {
            ns_timeleft = update_timeout( *end ) * 100;
            if (!ns_timeleft) return STATUS_TIMEOUT;
        }
        SEVO_STAT( msync_direct_parks );
        ret = ulock_park( obj_shm, val, ns_timeleft );
    } while (ret == -EINTR || ret == -EFAULT);

    if (ret == -ETIMEDOUT)
        return STATUS_TIMEOUT;

    return STATUS_SUCCESS;
}

/* A thread's word in shm_tid_map while it has a wait registered with the server: a state
 * in the low four bits and, for a WaitAll, the registration's generation above them. The
 * thread writes ACK_PENDING before it sends the registration and ACK_PARKED when it goes
 * to sleep waiting for the acknowledgment; the server acknowledges with TOKEN_REGISTERED,
 * or TOKEN_WOKEN when a wait-any is already satisfied, and wakes the thread only when what
 * it replaced was ACK_PARKED. A WaitAll ends in one of the terminal values: the pump
 * writes the alert by compare-and-swap from TOKEN_REGISTERED of the same generation, and
 * the thread cancels by compare-and-swap from any of the three live values, so exactly
 * one of them wins. A grant takes two steps: the pump moves the token from
 * TOKEN_REGISTERED to TOKEN_COMMITTING by compare-and-swap, consumes the set, and then
 * stores TOKEN_GRANTED or TOKEN_GRANTED_ABANDONED. TOKEN_COMMITTING is neither live nor
 * terminal: the thread cannot cancel out of it, and it waits for the value that follows,
 * so the success it returns is of a set already consumed and a query it makes next reads
 * the consumed objects. The generation is never zero, so a wait-any registration, which has
 * none, and a WaitAll cannot be mistaken for each other. server/msync.c has the same
 * values. */
#define TOKEN_WOKEN              0
#define TOKEN_REGISTERED         1
#define ACK_PENDING              2
#define ACK_PARKED               3
#define TOKEN_GRANTED            4
#define TOKEN_GRANTED_ABANDONED  5
#define TOKEN_CANCELLED          6
#define TOKEN_ALERTED            7
#define TOKEN_COMMITTING         8
#define TOKEN_STATE_MASK         0xf
#define TOKEN_GENERATION_SHIFT   4
#define TOKEN_GENERATION_MAX     ((1u << 28) - 1)

static inline int token_state( int value )
{
    return value & TOKEN_STATE_MASK;
}

static inline BOOL token_live( int value )
{
    int state = token_state( value );
    return state == TOKEN_REGISTERED || state == ACK_PENDING || state == ACK_PARKED;
}

static inline int token_with_state( int value, int state )
{
    return (value & ~TOKEN_STATE_MASK) | state;
}

/* Looks at a committing token this many times before parking on it. The pump's commit is a
 * handful of stores under the freeze, then the grant and a wake of this word. */
#define COMMIT_SPIN 4096

/* Sleeps until the pump has stored the grant over a TOKEN_COMMITTING token; the caller
 * reloads the token afterwards. */
static void wait_for_grant( int *addr, int seen )
{
    int i, ret;

    SEVO_STAT( waitall_commit_waits );
    for (i = 0; i < COMMIT_SPIN; i++)
    {
        YIELD_PROCESSOR;
        if (__atomic_load_n( addr, __ATOMIC_ACQUIRE ) != seen) return;
    }
    do ret = ulock_park( addr, seen, 0 );
    while (ret == -EINTR || ret == -EFAULT);
}

#ifndef SEVO_ACK_SPIN_DEFAULT
#define SEVO_ACK_SPIN_DEFAULT 4096
#endif

#define SEVO_ACK_SPIN_MAX 2000000000

/* Turns of the acknowledgment spin before it parks. SEVO_ACK_SPIN overrides it. */
static unsigned int ack_spin_budget(void)
{
    /* Published atomically: every thread that races here computes the same value. */
    static int budget = -1;
    int value = __atomic_load_n( &budget, __ATOMIC_RELAXED );

    if (value < 0)
    {
        value = sevo_env_budget( "SEVO_ACK_SPIN", SEVO_ACK_SPIN_DEFAULT, SEVO_ACK_SPIN_MAX );
        __atomic_store_n( &budget, value, __ATOMIC_RELAXED );
    }
    return value;
}

static inline int check_shm_contention( void **objs_shm, void *alert_obj_shm, int count, int tid )
{
    int i, val;

    for (i = 0; i < count; i++)
    {
        val = __atomic_load_n((int *)objs_shm[i], __ATOMIC_SEQ_CST);
        if (((struct mutex *)objs_shm[i])->msync_type == MSYNC_MUTEX)
        {
            if (val == 0 || val == ~0 || val == tid) return 1;
        }
        else
        {
            if (val != 0)  return 1;
        }
    }

    if (alert_obj_shm)
    {
        val = __atomic_load_n((int *)alert_obj_shm, __ATOMIC_SEQ_CST);
        if (val != 0)  return 1;
    }

    return 0;
}

static NTSTATUS msync_wait_multiple( const int *objs, void **objs_shm, int alert_obj, void *alert_obj_shm,
                                     int count, ULONGLONG *end, int tid )
{
    int ret, val;
    int *addr = shm_tid_map + tid;
    ULONGLONG ns_timeleft = 0;
    mach_msg_return_t mr;
    unsigned int msgh_id;
    int total_count = count + (alert_obj ? 1 : 0);

    __atomic_store_n( addr, ACK_PENDING, __ATOMIC_RELEASE );
    msgh_id = (tid << 8) | total_count;
    mr = server_register_wait( tid, objs, objs_shm, alert_obj, alert_obj_shm, count, 0 );

    if (mr != MACH_MSG_SUCCESS)
        return STATUS_PENDING;
    SEVO_STAT( msync_registrations );

    /* Waiting for the server to acknowledge the registration. A short look, rechecking
     * the objects every 64th turn; then the word is armed and the wait parks, because a
     * thread spinning here competes for a core with the pump it is waiting for. */
    {
        unsigned int turn = 0, budget = ack_spin_budget();

        while ((val = __atomic_load_n( addr, __ATOMIC_ACQUIRE )) == ACK_PENDING || val == ACK_PARKED)
        {
            if (turn >= budget)
            {
                int expected = ACK_PENDING;

                if (val == ACK_PENDING)
                {
                    if (!__atomic_compare_exchange_n( addr, &expected, ACK_PARKED, 0,
                                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ))
                        continue;
                    SEVO_STAT( msync_ack_parks );
                    SEVO_STAT_ADD( msync_ack_turns, turn );
                }
                if (end)
                {
                    ns_timeleft = update_timeout( *end ) * 100;
                    if (!ns_timeleft)
                    {
                        server_remove_wait( msgh_id, objs, objs_shm, alert_obj, alert_obj_shm, count );
                        return STATUS_TIMEOUT;
                    }
                }
                ulock_park( addr, ACK_PARKED, ns_timeleft );
                continue;
            }

            YIELD_PROCESSOR;
            if (!(++turn & 0x3f))
            {
                if (check_shm_contention( objs_shm, alert_obj_shm, count, tid ))
                {
                    SEVO_STAT( msync_ack_contention );
                    SEVO_STAT_ADD( msync_ack_turns, turn );
                    /* The registration is already submitted: the removal makes the
                     * server drop its nodes, and the interest counts drop with it. */
                    server_remove_wait( msgh_id, objs, objs_shm, alert_obj, alert_obj_shm, count );
                    return STATUS_PENDING;
                }
                if (end && !update_timeout( *end ))
                {
                    server_remove_wait( msgh_id, objs, objs_shm, alert_obj, alert_obj_shm, count );
                    return STATUS_TIMEOUT;
                }
            }
        }
        if (turn < budget) SEVO_STAT_ADD( msync_ack_turns, turn );
    }

    do
    {
        if (end)
        {
            ns_timeleft = update_timeout( *end ) * 100;
            if (!ns_timeleft)
            {
                server_remove_wait( msgh_id, objs, objs_shm, alert_obj, alert_obj_shm, count );
                return STATUS_TIMEOUT;
            }
        }
        SEVO_STAT( msync_registered_parks );
        ret = ulock_park( addr, TOKEN_REGISTERED, ns_timeleft );
        val = __atomic_load_n( addr, __ATOMIC_ACQUIRE );
        if (!val)
            break;
    } while (ret == -EINTR || ret == -EFAULT);

    server_remove_wait( msgh_id, objs, objs_shm, alert_obj, alert_obj_shm, count );

    if (ret == -ETIMEDOUT) return STATUS_TIMEOUT;

    return STATUS_SUCCESS;
}

/* TRUE when a member is unavailable to this thread at the instant of the look, which is a
 * legitimate answer to a poll and saves it the round trip; only the pump can say yes. */
static BOOL set_visibly_unavailable( void **objs_shm, int count, int tid )
{
    int i, val;

    for (i = 0; i < count; i++)
    {
        val = __atomic_load_n( (int *)objs_shm[i], __ATOMIC_SEQ_CST );
        if (((struct mutex *)objs_shm[i])->msync_type == MSYNC_MUTEX)
        {
            if (val && val != ~0 && val != tid) return TRUE;
        }
        else if (!val) return TRUE;
    }
    return FALSE;
}

/* Moves the token from a live value to TOKEN_CANCELLED. FALSE when the pump reached a
 * terminal value first, which *value then holds: a grant found this way stands, and one
 * the pump is still committing is waited for. */
static BOOL cancel_token( int *addr, int *value )
{
    int val = __atomic_load_n( addr, __ATOMIC_SEQ_CST );

    for (;;)
    {
        if (token_live( val ))
        {
            if (__atomic_compare_exchange_n( addr, &val, token_with_state( val, TOKEN_CANCELLED ), 0,
                                             __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ))
                return TRUE;
            continue;
        }
        if (token_state( val ) != TOKEN_COMMITTING) break;
        wait_for_grant( addr, val );
        val = __atomic_load_n( addr, __ATOMIC_SEQ_CST );
    }
    *value = val;
    return FALSE;
}

/* One WaitAll: the set is registered with the pump, which freezes every member, grants
 * the set only when all are available at one instant and consumes them itself, and
 * re-evaluates the registration whenever a member is signaled. The thread sleeps on its
 * token until the pump writes a terminal value or the deadline passes. The deadline is
 * looked at only once the pump has answered the registration, so a poll gets the pump's
 * verdict on the set as it was, never a timeout for a set that was whole. STATUS_PENDING
 * is a wake that carried no answer; the caller registers again. */
static NTSTATUS msync_wait_all( const int *objs, void **objs_shm, int alert_obj, void *alert_obj_shm,
                                int count, ULONGLONG *end, int tid )
{
    /* Each registration of this thread has its own generation, so the pump's answer to an
     * earlier one it processes late cannot be taken for, or acknowledge, this one. */
    static __thread unsigned int generation;
    int *addr = shm_tid_map + tid;
    int val, ret;
    unsigned int msgh_id, turn = 0, budget = ack_spin_budget();
    ULONGLONG ns_timeleft = 0;
    mach_msg_return_t mr;

    if (++generation > TOKEN_GENERATION_MAX) generation = 1;
    __atomic_store_n( addr, (int)(generation << TOKEN_GENERATION_SHIFT) | ACK_PENDING, __ATOMIC_RELEASE );
    msgh_id = (tid << 8) | (count + (alert_obj ? 1 : 0));
    mr = server_register_wait( tid, objs, objs_shm, alert_obj, alert_obj_shm, count, generation );
    if (mr != MACH_MSG_SUCCESS) return STATUS_PENDING;
    SEVO_STAT( waitall_registrations );

    for (;;)
    {
        val = __atomic_load_n( addr, __ATOMIC_ACQUIRE );
        if (token_state( val ) == TOKEN_COMMITTING)
        {
            /* The pump is consuming the set for this thread; the grant follows. */
            wait_for_grant( addr, val );
            continue;
        }
        if (!token_live( val )) break;

        if (token_state( val ) == ACK_PENDING)
        {
            if (turn < budget)
            {
                YIELD_PROCESSOR;
                turn++;
                continue;
            }
            if (!__atomic_compare_exchange_n( addr, &val, token_with_state( val, ACK_PARKED ), 0,
                                              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ))
                continue;
            SEVO_STAT( msync_ack_parks );
            SEVO_STAT_ADD( msync_ack_turns, turn );
            val = token_with_state( val, ACK_PARKED );
        }

        if (token_state( val ) == TOKEN_REGISTERED)
        {
            if (end)
            {
                ns_timeleft = update_timeout( *end ) * 100;
                if (!ns_timeleft) goto cancel;
            }
            SEVO_STAT( msync_registered_parks );
        }
        else ns_timeleft = 0;

        ret = ulock_park( addr, val, ns_timeleft );
        if (ret == -ETIMEDOUT) goto cancel;
    }
    goto terminal;

cancel:
    if (cancel_token( addr, &val ))
    {
        SEVO_STAT( waitall_cancels );
        server_remove_wait( msgh_id, objs, objs_shm, alert_obj, alert_obj_shm, count );
        if (alert_obj_shm && __atomic_load_n( (int *)alert_obj_shm, __ATOMIC_SEQ_CST )) return STATUS_USER_APC;
        return STATUS_TIMEOUT;
    }

terminal:
    /* A granted set was consumed by the pump and is this thread's. */
    server_remove_wait( msgh_id, objs, objs_shm, alert_obj, alert_obj_shm, count );
    switch (token_state( val ))
    {
    case TOKEN_GRANTED:
        SEVO_STAT( waitall_grants );
        return STATUS_SUCCESS;
    case TOKEN_GRANTED_ABANDONED:
        SEVO_STAT( waitall_grants );
        return STATUS_ABANDONED;
    case TOKEN_ALERTED:
        return STATUS_USER_APC;
    default:
        /* TOKEN_WOKEN from an earlier registration of this thread, or a cancellation
         * written for a thread the server saw die: nothing was granted. */
        SEVO_STAT( waitall_stale_wakes );
        return STATUS_PENDING;
    }
}

int do_msync(void)
{
    static int do_msync_cached = -1;

    if (do_msync_cached == -1)
        do_msync_cached = getenv("WINEMSYNC") && atoi(getenv("WINEMSYNC"));

    return do_msync_cached;
}

static const mach_vm_size_t shm_tid_size = 64 * 1024 * 1024; /* 64 MB to index 24 bit tids */
static void **shm_addrs;
static int shm_addrs_size;  /* length of the allocated shm_addrs array */
/* Object pages the table can name: at 16 bytes an object, 67 million objects on 16 KB pages. */
#define MSYNC_SHM_PAGES_MAX 65536
static long pagesize;

typedef struct
{
    mach_msg_header_t header;
    int entry;
} mach_map_message_t;

typedef struct
{
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t descriptor;
    mach_msg_trailer_t trailer;
} mach_map_message_reply_t;

static void *request_shm_from_server( int entry, int tid )
{
    static __thread mach_map_message_t send_message;
    static __thread mach_map_message_reply_t receive_message;
    mach_msg_return_t mr;
    kern_return_t kr;
    mach_port_t reply_port;
    mach_vm_address_t map_address = 0;

    TRACE( "requesting shm entry %d from server\n", entry );

    kr = mach_port_allocate( mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply_port );

    if (kr != KERN_SUCCESS)
    {
        ERR( "Failed to allocate reply port: %s\n", mach_error_string( kr ) );
        return NULL;
    }

    kr = mach_port_insert_right( mach_task_self(), reply_port, reply_port, MACH_MSG_TYPE_MAKE_SEND );

    if (kr != KERN_SUCCESS)
    {
        ERR( "Failed to insert right into reply port: %s\n", mach_error_string( kr ) );
        mach_port_mod_refs( mach_task_self(), reply_port, MACH_PORT_RIGHT_RECEIVE, -1 );
        return NULL;
    }

    send_message.header.msgh_bits = MACH_MSGH_BITS_SET( MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_COPY_SEND, 0, 0 );
    send_message.header.msgh_id = tid;
    send_message.header.msgh_size = sizeof(send_message);
    send_message.header.msgh_remote_port = server_port;
    send_message.header.msgh_local_port = reply_port;
    send_message.entry = entry;

    mr = mach_msg_overwrite( &send_message.header, MACH_SEND_MSG | MACH_RCV_MSG,
               send_message.header.msgh_size, sizeof(receive_message), reply_port,
               MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL, &receive_message.header, 0 );

    if (mr != MACH_MSG_SUCCESS)
    {
        ERR( "Failed to send/receive shm map request: %#x\n", mr );
    }
    else
    {
        mach_vm_size_t size = tid ? shm_tid_size : pagesize;

        TRACE( "mapping shm entry %u with size %llu\n", receive_message.descriptor.name, size );

        kr = mach_vm_map( mach_task_self(), &map_address, size, 0, VM_FLAGS_ANYWHERE,
                          receive_message.descriptor.name, 0, FALSE, VM_PROT_DEFAULT,
                          VM_PROT_DEFAULT, VM_INHERIT_NONE );

        if (kr != KERN_SUCCESS)
        {
            ERR( "Failed to map shm entry: %u: %d (%s)\n", receive_message.descriptor.name, kr, mach_error_string( kr ) );
            map_address = 0;
        }
        /* Only a received reply carries a descriptor; the buffer is this thread's, and after
           a failed receive it still names the previous call's port. */
        mach_port_deallocate( mach_task_self(), receive_message.descriptor.name );
    }

    /* The port holds a send right and the receive right: both go. */
    mach_port_deallocate( mach_task_self(), reply_port );
    mach_port_mod_refs( mach_task_self(), reply_port, MACH_PORT_RIGHT_RECEIVE, -1 );
    return (void *)map_address;
}

static os_unfair_lock shm_addrs_lock = OS_UNFAIR_LOCK_INIT;

static void *get_shm_slow( unsigned int idx )
{
    int entry  = (idx * 16) / pagesize;
    int offset = (idx * 16) % pagesize;
    void *ret;

    os_unfair_lock_lock( &shm_addrs_lock );

    if (entry >= shm_addrs_size)
    {
        ERR("Object page %d is past the %d the table holds; the process ends.\n", entry, shm_addrs_size);
        abort_process( 1 );
    }

    if (!shm_addrs[entry])
    {
        void *addr = request_shm_from_server( entry, 0 );
        /* Every object on the page lives there: without it the caller would read and write
           at an offset from address zero. */
        if (!addr)
        {
            ERR("Failed to map page %d (offset %#lx); the process ends.\n", entry, entry * pagesize);
            abort_process( 1 );
        }

        TRACE("Mapping page %d at %p.\n", entry, addr);

        if (__sync_val_compare_and_swap( &shm_addrs[entry], 0, addr ))
            mach_vm_deallocate( mach_task_self(), (mach_vm_address_t)addr, pagesize ); /* someone beat us to it */
    }

    ret = (void *)((unsigned long)shm_addrs[entry] + offset);

    os_unfair_lock_unlock( &shm_addrs_lock );

    return ret;
}

static inline void *get_shm( const unsigned int idx )
{
    int entry = idx >> (vm_kernel_page_shift - 4);
    int offset = (idx << 4) & vm_kernel_page_mask;

    if (entry >= shm_addrs_size || !shm_addrs[entry])
        return get_shm_slow( idx );

    return (void *)((unsigned long)shm_addrs[entry] + offset);
}

void msync_close( int obj )
{
    static __thread mach_msg_header_t send_header;
    mach_msg_return_t mr;

    TRACE( "obj=%d.\n", obj );

    send_header.msgh_bits = MACH_MSGH_BITS_REMOTE(MACH_MSG_TYPE_COPY_SEND);
    send_header.msgh_id = obj | (1 << 28);
    send_header.msgh_size = sizeof(send_header);
    send_header.msgh_remote_port = server_port;

    mr = mach_msg2( &send_header, MACH_SEND_MSG, send_header.msgh_size,
                    0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, 0);

    if (mr != MACH_MSG_SUCCESS)
    {
        abort_if_server_gone( mr );
        ERR( "Failed to send message to server to close msync object %d: %#x\n", obj, mr );
    }
}

/* A thread parked on one object's word (msync_wait_single) sleeps in the kernel on shared
 * memory and never sends the server anything, so abort_if_server_gone cannot reach it:
 * with the wineserver dead, a process whose threads all wait that way lives on with no
 * server at all. This thread holds a dead-name notification for server_port and ends the
 * process when the port dies. It costs one parked thread and nothing while the server
 * lives. */
static void *server_death_watch( void *arg )
{
    mach_port_t notify_port = (mach_port_t)(uintptr_t)arg;
    struct
    {
        mach_dead_name_notification_t notification;
        mach_msg_trailer_t trailer;
    } message;
    mach_msg_return_t mr;
    char line[160];
    int len;

    for (;;)
    {
        mr = mach_msg( &message.notification.not_header, MACH_RCV_MSG, 0,
                       sizeof(message), notify_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL );
        if (mr == MACH_MSG_SUCCESS && message.notification.not_header.msgh_id == MACH_NOTIFY_DEAD_NAME)
            break;
        /* Only the kernel holds a right to this port; anything else it says carries no right. */
        if (mr != MACH_MSG_SUCCESS && mr != MACH_RCV_TOO_LARGE && mr != MACH_RCV_INTERRUPTED)
            return NULL;
    }
    /* A plain write: ERR needs the TEB of a Wine thread, which this thread has none of, and
     * blocks forever looking for it. */
    len = snprintf( line, sizeof(line), "%04x:err:sync:server_death_watch msync server is gone "
                    "(the wineserver died); the process ends\n", (unsigned int)getpid() );
    if (len > 0) write( 2, line, len < (int)sizeof(line) ? len : (int)sizeof(line) - 1 );
    abort_process( 1 );
}

static void watch_server_death(void)
{
    mach_port_t notify_port, previous = MACH_PORT_NULL;
    sigset_t all, saved;
    pthread_attr_t attr;
    pthread_t thread;
    kern_return_t kr;

    if (mach_port_allocate( mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &notify_port ) != KERN_SUCCESS)
    {
        ERR( "no port for the msync server watch\n" );
        return;
    }
    /* sync = 1: a port that is already dead is notified at once. */
    kr = mach_port_request_notification( mach_task_self(), server_port, MACH_NOTIFY_DEAD_NAME, 1,
                                         notify_port, MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous );
    if (kr != KERN_SUCCESS)
    {
        ERR( "msync server watch not armed: %#x\n", kr );
        mach_port_mod_refs( mach_task_self(), notify_port, MACH_PORT_RIGHT_RECEIVE, -1 );
        return;
    }
    if (previous != MACH_PORT_NULL) mach_port_deallocate( mach_task_self(), previous );

    /* The thread is not a Wine thread: it takes no signal meant for one. */
    sigfillset( &all );
    pthread_sigmask( SIG_BLOCK, &all, &saved );
    pthread_attr_init( &attr );
    pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
    pthread_attr_setstacksize( &attr, 64 * 1024 );
    if (pthread_create( &thread, &attr, server_death_watch, (void *)(uintptr_t)notify_port ))
        ERR( "msync server watch thread not started\n" );
    pthread_attr_destroy( &attr );
    pthread_sigmask( SIG_SETMASK, &saved, NULL );
}

void msync_init(void)
{
    struct stat st;
    mach_port_t bootstrap_port;
    void *dlhandle = dlopen( NULL, RTLD_NOW );
    char message_port_name[28];

    if (!do_msync())
    {
        /* make sure the server isn't running with WINEMSYNC */
        NTSTATUS ret;

        SERVER_START_REQ( get_inproc_alert_fd )
        {
            ret = wine_server_call( req );
        }
        SERVER_END_REQ;

        if (ret != STATUS_INVALID_PARAMETER)
        {
            ERR("Server is running with WINEMSYNC but this process is not, please enable WINEMSYNC or restart wineserver.\n");
            exit(1);
        }

        dlclose( dlhandle );
        return;
    }

    if (stat( config_dir, &st ) == -1)
    {
        ERR("Cannot stat %s\n", config_dir);
        exit(1);
    }

    if (st.st_ino != (unsigned long)st.st_ino)
        snprintf( message_port_name, 28, "wine-%lx%08lx-msync", (unsigned long)((unsigned long long)st.st_ino >> 32), (unsigned long)st.st_ino );
    else
        snprintf( message_port_name, 28, "wine-%lx-msync", (unsigned long)st.st_ino );

    pagesize = (long)vm_kernel_page_size;

    /* Allocated once and never moved: get_shm reads it without the lock. Untouched entries
       cost no memory until a page of objects lands there. */
    shm_addrs = calloc( MSYNC_SHM_PAGES_MAX, sizeof(shm_addrs[0]) );
    shm_addrs_size = shm_addrs ? MSYNC_SHM_PAGES_MAX : 0;

    /* Bootstrap mach wineserver communication */

    mach_msg2_trap = (mach_msg2_trap_ptr_t)dlsym( dlhandle, "mach_msg2_trap" );
    if (!mach_msg2_trap)
        WARN("Using mach_msg instead of mach_msg2\n");
    dlclose( dlhandle );

    if (task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &bootstrap_port) != KERN_SUCCESS)
    {
        ERR("Failed task_get_special_port\n");
        exit(1);
    }

    if (bootstrap_look_up(bootstrap_port, message_port_name, &server_port) != KERN_SUCCESS)
    {
        ERR("Failed bootstrap_look_up for %s\n", message_port_name);
        exit(1);
    }

    shm_tid_map = request_shm_from_server( 0, 1 );

    if (!shm_tid_map)
    {
        ERR("Failed to map tid shared memory");
        exit(1);
    }

    watch_server_death();
}

/* ENOENT is an object nobody sleeps on. An interrupted wake woke nobody and is made
 * again; any other failure leaves a sleeper nobody else will call for, so it is said. */
static inline void wake_all_on_word( void *shm )
{
    static unsigned int failures;
    int ret;

    do ret = __ulock_wake( UL_COMPARE_AND_WAIT_SHARED | ULF_WAKE_ALL, shm, 0 );
    while (ret == -1 && errno == EINTR);

    if (ret == -1 && errno != ENOENT && failures++ < 8)
        ERR( "wake of %p failed: errno %d\n", shm, errno );
}

static inline void signal_all( void *shm, unsigned int shm_idx )
{
    __thread static mach_msg_header_t send_header;
    struct event *event_obj = (struct event *)shm;
    mach_msg_return_t mr;

    SEVO_STAT( signal_all_calls );
    wake_all_on_word( shm );

    if (!__atomic_load_n( &event_obj->multiple_waiters, __ATOMIC_SEQ_CST ))
        return;
    SEVO_STAT( signal_all_messages );

    send_header.msgh_bits = msgh_bits_send;
    send_header.msgh_id = shm_idx;
    send_header.msgh_size = sizeof(send_header);
    send_header.msgh_remote_port = server_port;

    mr = mach_msg2( &send_header, MACH_SEND_MSG, send_header.msgh_size, 0,
                    MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, 0 );
    if (mr != MACH_MSG_SUCCESS)
    {
        abort_if_server_gone( mr );
        SEVO_STAT( signal_all_send_failures );
    }
}

NTSTATUS msync_release_semaphore_obj( int obj, ULONG count, ULONG *prev_count )
{
    struct semaphore *semaphore = get_shm( obj );
    uint64_t word = load_object( semaphore );
    ULONG current, max;

    for (;;)
    {
        word = thawed_object( semaphore, word );
        current = object_low( word );
        max = object_high( word );
        /* Subtracted rather than added: count + current wraps for a count near 2^32. */
        if (current > max || count > max - current)
            return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
        if (swap_object( semaphore, &word, make_object( current + count, max ) )) break;
    }

    if (prev_count) *prev_count = current;

    signal_all( (void *)semaphore, obj );
    return STATUS_SUCCESS;
}

/* A query reads the object once the pump is not consuming it: a WaitAll's members move
 * together under the freeze, and a read in the middle would show a set half taken. */
NTSTATUS msync_query_semaphore_obj( int obj, SEMAPHORE_BASIC_INFORMATION *info )
{
    struct semaphore *semaphore = get_shm( obj );
    uint64_t word = thawed_object( semaphore, load_object( semaphore ) );

    info->CurrentCount = object_low( word );
    info->MaximumCount = object_high( word );

    return STATUS_SUCCESS;
}

NTSTATUS msync_set_event_obj( int obj, LONG *prev_state )
{
    struct event *event = get_shm( obj );
    uint64_t word = load_object( event );
    LONG current;

    for (;;)
    {
        word = thawed_object( event, word );
        if ((current = object_low( word ))) break;
        if (swap_object( event, &word, make_object( 1, object_high( word ) ) ))
        {
            signal_all( (void *)event, obj );
            break;
        }
    }

    if (prev_state) *prev_state = current;

    return STATUS_SUCCESS;
}

NTSTATUS msync_reset_event_obj( int obj, LONG *prev_state )
{
    struct event *event = get_shm( obj );
    uint64_t word = load_object( event );
    LONG current;

    for (;;)
    {
        word = thawed_object( event, word );
        if (!(current = object_low( word ))) break;
        if (swap_object( event, &word, make_object( 0, object_high( word ) ) )) break;
    }

    if (prev_state) *prev_state = current;

    return STATUS_SUCCESS;
}

NTSTATUS msync_pulse_event_obj( int obj, LONG *prev_state )
{
    /* This isn't really correct; an application could miss the write.
     * Unfortunately we can't really do much better. Fortunately this is rarely
     * used (and publicly deprecated). */
    NTSTATUS status = msync_set_event_obj( obj, prev_state );

    /* Try to give other threads a chance to wake up. Hopefully erring on this
     * side is the better thing to do... */
    sched_yield();

    msync_reset_event_obj( obj, NULL );

    return status;
}

NTSTATUS msync_query_event_obj( int obj, EVENT_BASIC_INFORMATION *info )
{
    struct event *event = get_shm( obj );
    uint64_t word = thawed_object( event, load_object( event ) );

    info->EventState = object_low( word );
    info->EventType = (event->msync_type == MSYNC_AUTO_EVENT ? SynchronizationEvent : NotificationEvent);

    return STATUS_SUCCESS;
}

NTSTATUS msync_release_mutex_obj( int obj, LONG *prev_count )
{
    struct mutex *mutex = get_shm( obj );
    uint64_t word = load_object( mutex );
    int tid = GetCurrentThreadId();
    unsigned int depth;

    for (;;)
    {
        word = thawed_object( mutex, word );
        if (object_low( word ) != tid)
            return STATUS_MUTANT_NOT_OWNED;
        depth = object_high( word );
        if (swap_object( mutex, &word, depth > 1 ? make_object( tid, depth - 1 ) : make_object( 0, 0 ) ))
            break;
    }

    if (prev_count) *prev_count = 1 - (LONG)depth;

    if (depth <= 1) signal_all( (void *)mutex, obj );

    return STATUS_SUCCESS;
}

NTSTATUS msync_query_mutex_obj( int obj, MUTANT_BASIC_INFORMATION *info )
{
    struct mutex *mutex = get_shm( obj );
    uint64_t word = thawed_object( mutex, load_object( mutex ) );

    info->CurrentCount = 1 - (LONG)object_high( word );
    info->OwnedByCaller = (object_low( word ) == GetCurrentThreadId());
    info->AbandonedState = (object_low( word ) == ~0);

    return STATUS_SUCCESS;
}

NTSTATUS msync_wait_objs( const DWORD count, const int *objs, BOOLEAN wait_any,
                          int alert_obj, const LARGE_INTEGER *timeout )
{
    static const LARGE_INTEGER zero = {0};

    int current_tid = 0;
    static __thread void *objs_shm[MAXIMUM_WAIT_OBJECTS];
    int *alert_obj_shm = NULL;
    LARGE_INTEGER now;
    int single_wait = 0;
    ULONGLONG end;
    int i, ret;

    SEVO_STAT( msync_waits );

    current_tid = GetCurrentThreadId();

    if (alert_obj)
    {
        alert_obj_shm = get_shm( alert_obj );
        if (!count) single_wait = 1;
    }
    else
    {
        if (count == 1) single_wait = 1;
    }

    NtQuerySystemTime( &now );
    if (timeout)
    {
        if (timeout->QuadPart == TIMEOUT_INFINITE)
            timeout = NULL;
        else if (timeout->QuadPart > 0)
            end = timeout->QuadPart;
        else
            end = now.QuadPart - timeout->QuadPart;
    }

    for (i = 0; i < count; i++)
        objs_shm[i] = (struct event *)get_shm( objs[i] );

    if (wait_any || count <= 1)
    {
        while (1)
        {
            /* Try to grab anything. */

            if (alert_obj)
            {
                /* We must check this first! The server may set an event that
                 * we're waiting on, but we need to return STATUS_USER_APC. */
                if (__atomic_load_n( alert_obj_shm, __ATOMIC_SEQ_CST ))
                    goto userapc;
            }

            for (i = 0; i < count; i++)
            {
                uint64_t word = load_object( objs_shm[i] );

                switch (((struct event *)objs_shm[i])->msync_type)
                {
                case MSYNC_SEMAPHORE:
                    for (;;)
                    {
                        word = thawed_object( objs_shm[i], word );
                        if (!object_low( word )) break;
                        if (swap_object( objs_shm[i], &word, make_object( object_low( word ) - 1, object_high( word ) ) ))
                            return i;
                    }
                    break;
                case MSYNC_MUTEX:
                    for (;;)
                    {
                        int owner;

                        word = thawed_object( objs_shm[i], word );
                        owner = object_low( word );
                        if (owner == current_tid)
                        {
                            if (object_high( word ) == MSYNC_MUTEX_RECURSION_MAX)
                                return STATUS_MUTANT_LIMIT_EXCEEDED;
                            if (swap_object( objs_shm[i], &word, make_object( current_tid, object_high( word ) + 1 ) ))
                                return i;
                            continue;
                        }
                        /* An abandoned mutex (~0) is available to whoever grabs it. */
                        if (owner && owner != ~0) break;
                        if (swap_object( objs_shm[i], &word, make_object( current_tid, 1 ) ))
                            return owner == ~0 ? STATUS_ABANDONED_WAIT_0 + i : i;
                    }
                    break;
                case MSYNC_AUTO_EVENT:
                case MSYNC_AUTO_SERVER:
                    for (;;)
                    {
                        word = thawed_object( objs_shm[i], word );
                        if (!object_low( word )) break;
                        if (swap_object( objs_shm[i], &word, make_object( 0, object_high( word ) ) ))
                            return i;
                    }
                    break;
                case MSYNC_MANUAL_EVENT:
                case MSYNC_MANUAL_SERVER:
                    if (object_low( word ))
                        return i;
                    break;
                default:
                    ERR("Invalid type %#x for obj %d.\n", ((struct event *)objs_shm[i])->msync_type, objs[i]);
                    assert(0);
                }
            }

            /* Looks like everything is contended, so wait. */

            if (timeout && !timeout->QuadPart)
            {
                /* Unlike esync, we already know that we've timed out, so we
                 * can avoid a syscall. */
                return STATUS_TIMEOUT;
            }

            if (alert_obj && single_wait)
                ret = msync_wait_single( alert_obj, alert_obj_shm, timeout ? &end : NULL, current_tid );
            else if (single_wait)
                ret = msync_wait_single( objs[0], objs_shm[0], timeout ? &end : NULL, current_tid );
            else
                ret = msync_wait_multiple( objs, objs_shm, alert_obj, alert_obj_shm, count, timeout ? &end : NULL, current_tid );

            if (ret == STATUS_TIMEOUT) return STATUS_TIMEOUT;
        } /* while (1) */
    }
    else
    {
        /* A WaitAll is granted and consumed by the wineserver's pump under a freeze of the
         * whole set, so the set it reports was whole at one instant. This side only
         * registers, sleeps and reads the verdict; a poll that can see a missing member
         * is refused here without the round trip. */
        SEVO_STAT( waitall_calls );

        for (;;)
        {
            if (alert_obj && __atomic_load_n( alert_obj_shm, __ATOMIC_SEQ_CST ))
                goto userapc;

            if (timeout && !update_timeout( end ) && set_visibly_unavailable( objs_shm, count, current_tid ))
            {
                SEVO_STAT( waitall_polls_refused );
                return STATUS_TIMEOUT;
            }

            ret = msync_wait_all( objs, objs_shm, alert_obj, alert_obj_shm, count, timeout ? &end : NULL, current_tid );
            if (ret == STATUS_USER_APC) goto userapc;
            if (ret != STATUS_PENDING) return ret;
        }
    } /* else (wait-all) */

    assert(0);  /* shouldn't reach here... */

userapc:
    /* We have to make a server call anyway to get the APC to execute, so just
     * delegate down to server_wait(). */
    ret = server_wait( NULL, 0, SELECT_INTERRUPTIBLE | SELECT_ALERTABLE, &zero );

    /* This can happen if we received a system APC, and the APC fd was woken up
     * before we got SIGUSR1. poll() doesn't return EINTR in that case. The
     * right thing to do seems to be to return STATUS_USER_APC anyway. */
    if (ret == STATUS_TIMEOUT) ret = STATUS_USER_APC;
    return ret;
}

#else /* __APPLE__ */

int do_msync(void)
{
    return 0;
}

void msync_init(void)
{
}

void msync_close( int obj )
{
}

#endif /* __APPLE__ */
