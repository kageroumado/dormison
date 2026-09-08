#elif defined(__APPLE__)

#include <fcntl.h>

int get_inproc_device_fd(void)
{
    static int fd = -2;
    if (!do_msync()) return -1;
    if (fd == -2) fd = open( "/dev/null", O_CLOEXEC | O_RDONLY );
    return fd;
}

struct inproc_sync
{
    struct object          obj;
    enum inproc_sync_type  type;
    struct msync          *msync;
};

static void inproc_sync_dump( struct object *obj, int verbose );
static int inproc_sync_signal( struct object *obj, unsigned int access, int signal );
static void inproc_sync_destroy( struct object *obj );

static const struct object_ops inproc_sync_ops =
{
    sizeof(struct inproc_sync), /* size */
    &no_type,                   /* type */
    inproc_sync_dump,           /* dump */
    no_add_queue,               /* add_queue */
    NULL,                       /* remove_queue */
    NULL,                       /* signaled */
    NULL,                       /* satisfied */
    inproc_sync_signal,         /* signal */
    no_get_fd,                  /* get_fd */
    default_get_sync,           /* get_sync */
    default_map_access,         /* map_access */
    default_get_sd,             /* get_sd */
    default_set_sd,             /* set_sd */
    default_get_full_name,      /* get_full_name */
    no_lookup_name,             /* lookup_name */
    directory_link_name,        /* link_name */
    default_unlink_name,        /* unlink_name */
    no_open_file,               /* open_file */
    no_kernel_obj_list,         /* get_kernel_obj_list */
    no_close_handle,            /* close_handle */
    inproc_sync_destroy,        /* destroy */
};

int get_inproc_sync_fd( struct inproc_sync *sync )
{
    if (!sync) return -1;
    if (do_msync()) return (int)sync->msync->shm_idx;
    return -1;
}

struct inproc_sync *create_inproc_internal_sync( int manual, int signaled )
{
    struct inproc_sync *event;

    if (!(event = alloc_object( &inproc_sync_ops ))) return NULL;
    event->type = INPROC_SYNC_INTERNAL;
    event->msync = create_msync( signaled, 0xdeadbeef, manual ? MSYNC_MANUAL_SERVER : MSYNC_AUTO_SERVER );

    if (!event->msync)
    {
        set_error( STATUS_NO_MEMORY );
        release_object( event );
        return NULL;
    }
    return event;
}

struct inproc_sync *create_inproc_event_sync( int manual, int signaled )
{
    struct inproc_sync *event;

    if (!(event = alloc_object( &inproc_sync_ops ))) return NULL;
    event->type = INPROC_SYNC_EVENT;
    event->msync = create_msync( signaled, 0xdeadbeef, manual ? MSYNC_MANUAL_EVENT : MSYNC_AUTO_EVENT );

    if (!event->msync)
    {
        set_error( STATUS_NO_MEMORY );
        release_object( event );
        return NULL;
    }
    return event;
}

struct inproc_sync *create_inproc_mutex_sync( thread_id_t owner, unsigned int count )
{
    struct inproc_sync *mutex;

    if (!(mutex = alloc_object( &inproc_sync_ops ))) return NULL;
    mutex->type = INPROC_SYNC_MUTEX;
    mutex->msync = create_msync( owner, count, MSYNC_MUTEX);

    if (!mutex->msync)
    {
        set_error( STATUS_NO_MEMORY );
        release_object( mutex );
        return NULL;
    }
    return mutex;
}

struct inproc_sync *create_inproc_semaphore_sync( unsigned int initial, unsigned int max )
{
    struct inproc_sync *sem;

    if (!(sem = alloc_object( &inproc_sync_ops ))) return NULL;
    sem->type = INPROC_SYNC_SEMAPHORE;
    sem->msync = create_msync( initial, max, MSYNC_SEMAPHORE );

    if (!sem->msync)
    {
        set_error( STATUS_NO_MEMORY );
        release_object( sem );
        return NULL;
    }
    return sem;
}

static void inproc_sync_dump( struct object *obj, int verbose )
{
    struct inproc_sync *sync = (struct inproc_sync *)obj;
    assert( obj->ops == &inproc_sync_ops );

    fprintf( stderr, "Inproc sync type=%d, msync idx=%d\n", sync->type, sync->msync->shm_idx );
}

void signal_inproc_sync( struct inproc_sync *sync )
{
    if (debug_level) fprintf( stderr, "set_inproc_event %d\n", sync->msync->shm_idx );
    msync_set_event( sync->msync );
}

void reset_inproc_sync( struct inproc_sync *sync )
{
    if (debug_level) fprintf( stderr, "reset_inproc_event %d\n", sync->msync->shm_idx );
    msync_reset_event( sync->msync );
}

static int inproc_sync_signal( struct object *obj, unsigned int access, int signal )
{
    struct inproc_sync *sync = (struct inproc_sync *)obj;
    assert( obj->ops == &inproc_sync_ops );

    assert( sync->type == INPROC_SYNC_INTERNAL || sync->type == INPROC_SYNC_EVENT ); /* never called for mutex / semaphore */
    assert( signal == 0 || signal == 1 ); /* never called from signal_object */

    if (signal) signal_inproc_sync( sync );
    else reset_inproc_sync( sync );
    return 1;
}

static void inproc_sync_destroy( struct object *obj )
{
    struct inproc_sync *sync = (struct inproc_sync *)obj;
    assert( obj->ops == &inproc_sync_ops );
    msync_destroy( sync->msync );
}

void abandon_inproc_mutexes( thread_id_t tid )
{
    if (do_msync()) msync_abandon_mutexes( tid );
}

static int get_obj_inproc_sync( struct object *obj, int *type )
{
    struct object *sync;
    int shm_idx = -1;

    if (!do_msync()) return -1;
    if (!(sync = get_obj_sync( obj ))) return -1;
    if (sync->ops == &inproc_sync_ops)
    {
        struct inproc_sync *inproc = (struct inproc_sync *)sync;
        msync_grab_object( inproc->msync );
        *type = inproc->type;
        shm_idx = (int)inproc->msync->shm_idx;
    }

    release_object( sync );
    return shm_idx;
}

