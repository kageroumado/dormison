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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "sevo_stats.h"

/* The page's hot fields, at the offsets sevo_stats_page names. A reader is
   another process on another architecture, so every one of them is a plain
   aligned 64-bit word. */
struct stats_page
{
    _Atomic uint64_t magic;
    _Atomic uint64_t frames;
    _Atomic uint64_t drawables;
    _Atomic uint64_t last_present_ns;
    uint64_t         start_ns;
    _Atomic uint64_t window_id;
    uint32_t         version;
    uint32_t         pid;
    _Atomic uint32_t source;
    uint32_t         appid;
    char             exe[32];
    _Atomic uint64_t main_beat_ns;
    _Atomic uint64_t ring_head;
    uint32_t         ring_capacity;
    uint32_t         ring_reserved;
    _Atomic uint32_t ring[SEVO_STATS_RING_CAPACITY];
};

_Static_assert(sizeof(struct stats_page) == sizeof(struct sevo_stats_page),
               "the written page and the documented page are one layout");
_Static_assert(sizeof(struct stats_page) <= SEVO_STATS_PAGE_SIZE, "the page fits its page");
_Static_assert(offsetof(struct stats_page, ring_head) == 104 && offsetof(struct stats_page, ring) == 120,
               "the app reads the ring at these offsets");

static struct stats_page *_Atomic page;
static pthread_once_t page_once = PTHREAD_ONCE_INIT;
static char page_path[PATH_MAX];
static uint32_t process_appid;
static char process_exe[32];

static uint64_t uptime_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/* The prefix this process runs in, the way ntdll's loader resolves it. */
static const char *prefix_path(void)
{
    const char *prefix = getenv("WINEPREFIX");

    if (prefix && prefix[0] == '/') return prefix;
    return NULL;
}

/* Makes the directory and every parent of it under the prefix. */
static int make_run_directory(char *out, size_t size)
{
    const char *prefix = prefix_path();
    char path[PATH_MAX];

    if (!prefix) return 0;
    if (snprintf(path, sizeof(path), "%s/.sevo", prefix) >= (int)sizeof(path)) return 0;
    if (mkdir(path, 0700) && errno != EEXIST) return 0;
    if (snprintf(path, sizeof(path), "%s/.sevo/run", prefix) >= (int)sizeof(path)) return 0;
    if (mkdir(path, 0700) && errno != EEXIST) return 0;
    return snprintf(out, size, "%s", path) < (int)size;
}

/* Drops the pages of processes that are gone. A process killed outright
   never runs its own cleanup, so the directory is swept by the next one to
   present rather than by whoever left the file. */
static void sweep_dead_pages(const char *directory)
{
    struct dirent *entry;
    DIR *dir = opendir(directory);

    if (!dir) return;
    while ((entry = readdir(dir)))
    {
        char path[PATH_MAX], *end;
        long value;

        value = strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || strcmp(end, ".stats")) continue;
        if (value <= 0 || value == (long)getpid()) continue;
        if (!kill((pid_t)value, 0) || errno != ESRCH) continue;
        if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) < (int)sizeof(path))
            unlink(path);
    }
    closedir(dir);
}

static void close_page(void)
{
    if (page_path[0]) unlink(page_path);
}

static void open_page(void)
{
    char directory[PATH_MAX];
    struct stats_page *mapped;
    int fd;

    if (!make_run_directory(directory, sizeof(directory))) return;
    sweep_dead_pages(directory);
    if (snprintf(page_path, sizeof(page_path), "%s/%d.stats", directory, getpid())
        >= (int)sizeof(page_path))
    {
        page_path[0] = 0;
        return;
    }

    fd = open(page_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
    {
        page_path[0] = 0;
        return;
    }
    if (ftruncate(fd, SEVO_STATS_PAGE_SIZE))
    {
        close(fd);
        unlink(page_path);
        page_path[0] = 0;
        return;
    }
    mapped = mmap(NULL, SEVO_STATS_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (mapped == MAP_FAILED)
    {
        unlink(page_path);
        page_path[0] = 0;
        return;
    }

    mapped->version = SEVO_STATS_VERSION;
    mapped->pid = (uint32_t)getpid();
    mapped->appid = process_appid;
    mapped->start_ns = uptime_ns();
    atomic_store_explicit(&mapped->main_beat_ns, mapped->start_ns, memory_order_relaxed);
    memcpy(mapped->exe, process_exe, sizeof(mapped->exe));
    mapped->ring_capacity = SEVO_STATS_RING_CAPACITY;
    /* Last, and with release ordering: the magic is what says the rest of
       the page is there to be read. */
    atomic_store_explicit(&mapped->magic, SEVO_STATS_MAGIC, memory_order_release);
    atomic_store_explicit(&page, mapped, memory_order_release);
    atexit(close_page);
}

/* Notes one counted frame in the ring. Relaxed: the reader tolerates a slot it reads while it
   is written by stopping one short of the head. */
static void push_frame(struct stats_page *current_page, uint64_t now)
{
    uint64_t index = atomic_fetch_add_explicit(&current_page->ring_head, 1, memory_order_relaxed);
    uint32_t micros = (uint32_t)((now - current_page->start_ns) / 1000);

    atomic_store_explicit(&current_page->ring[index % SEVO_STATS_RING_CAPACITY], micros, memory_order_relaxed);
}

/* The Steam app this process belongs to, 0 when it is not a game's. */
unsigned int sevo_stats_appid(void)
{
    return process_appid;
}

void sevo_stats_init(unsigned int appid, const char *exe)
{
    process_appid = appid;
    snprintf(process_exe, sizeof(process_exe), "%s", exe ? exe : "");
}

void sevo_stats_note_present(unsigned int source)
{
    struct stats_page *current_page;
    unsigned int current;
    uint64_t now;

    pthread_once(&page_once, open_page);
    if (!(current_page = atomic_load_explicit(&page, memory_order_acquire))) return;

    /* One path counts. The presenter takes over from the client surface the
       moment it presents, because it is the one whose frames reach the
       screen; a lower path arriving afterwards is the same frame counted
       twice. */
    current = atomic_load_explicit(&current_page->source, memory_order_relaxed);
    if (current > source) return;
    if (current < source) atomic_store_explicit(&current_page->source, source, memory_order_relaxed);

    now = uptime_ns();
    atomic_store_explicit(&current_page->last_present_ns, now, memory_order_relaxed);
    atomic_fetch_add_explicit(&current_page->frames, 1, memory_order_relaxed);
    /* The ring follows the count the rate is read from: the presenter's frames, else
       D3DMetal's drawables, which push_frame takes in sevo_stats_note_drawable. */
    if (source == SEVO_STATS_SOURCE_PRESENTER
        || !atomic_load_explicit(&current_page->drawables, memory_order_relaxed))
        push_frame(current_page, now);
}

void sevo_stats_note_drawable(void)
{
    struct stats_page *current_page;

    pthread_once(&page_once, open_page);
    if (!(current_page = atomic_load_explicit(&page, memory_order_acquire))) return;
    atomic_fetch_add_explicit(&current_page->drawables, 1, memory_order_relaxed);
    if (atomic_load_explicit(&current_page->source, memory_order_relaxed) != SEVO_STATS_SOURCE_PRESENTER)
        push_frame(current_page, uptime_ns());
}

void sevo_stats_note_main_beat(void)
{
    struct stats_page *current_page = atomic_load_explicit(&page, memory_order_acquire);

    if (current_page) atomic_store_explicit(&current_page->main_beat_ns, uptime_ns(), memory_order_relaxed);
}

void sevo_stats_note_window(unsigned long long window_id)
{
    struct stats_page *current_page = atomic_load_explicit(&page, memory_order_acquire);
    uint64_t none = 0;

    if (!current_page || !window_id) return;
    atomic_compare_exchange_strong_explicit(&current_page->window_id, &none, (uint64_t)window_id,
                                            memory_order_relaxed, memory_order_relaxed);
}

/* Frames so far and the window they go to, for the frame-rate counter the
   driver draws itself: the count the app and the frame ring read. The
   presenter's frames when it presents; otherwise drawables whenever D3DMetal
   has taken any, since its presents are coalesced and its drawables never are. */
unsigned long long sevo_stats_frame_count(unsigned long long *window_id)
{
    struct stats_page *current_page = atomic_load_explicit(&page, memory_order_acquire);
    uint64_t drawables;

    if (window_id) *window_id = 0;
    if (!current_page) return 0;
    if (window_id) *window_id = atomic_load_explicit(&current_page->window_id, memory_order_relaxed);
    if (atomic_load_explicit(&current_page->source, memory_order_relaxed) == SEVO_STATS_SOURCE_PRESENTER)
        return atomic_load_explicit(&current_page->frames, memory_order_relaxed);
    drawables = atomic_load_explicit(&current_page->drawables, memory_order_relaxed);
    return drawables ? drawables : atomic_load_explicit(&current_page->frames, memory_order_relaxed);
}

unsigned int sevo_stats_recent_frames(unsigned int *out, unsigned int max)
{
    struct stats_page *current_page = atomic_load_explicit(&page, memory_order_acquire);
    uint64_t head, first, index;
    unsigned int count = 0;

    if (!current_page || !max) return 0;
    /* One short of the head, where a slot may still be being written, and a few short of a
       full ring at the tail, where the next frames overwrite the oldest slots. */
    head = atomic_load_explicit(&current_page->ring_head, memory_order_relaxed);
    if (head < 2) return 0;
    head -= 1;
    first = head > SEVO_STATS_RING_CAPACITY - 8 ? head - (SEVO_STATS_RING_CAPACITY - 8) : 0;
    if (head - first > max) first = head - max;
    for (index = first; index < head; index++)
        out[count++] = atomic_load_explicit(&current_page->ring[index % SEVO_STATS_RING_CAPACITY],
                                            memory_order_relaxed);
    return count;
}
