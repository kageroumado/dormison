#elif defined(__APPLE__)

static NTSTATUS linux_release_semaphore_obj( int obj, ULONG count, ULONG *prev_count )
{
    return msync_release_semaphore_obj( obj, count, prev_count );
}

static NTSTATUS linux_query_semaphore_obj( int obj, SEMAPHORE_BASIC_INFORMATION *info )
{
    return msync_query_semaphore_obj( obj, info );
}

static NTSTATUS linux_set_event_obj( int obj, LONG *prev_state )
{
    return msync_set_event_obj( obj, prev_state );
}

static NTSTATUS linux_reset_event_obj( int obj, LONG *prev_state )
{
    return msync_reset_event_obj( obj, prev_state );
}

static NTSTATUS linux_pulse_event_obj( int obj, LONG *prev_state )
{
    return msync_pulse_event_obj( obj, prev_state );
}

static NTSTATUS linux_query_event_obj( int obj, EVENT_BASIC_INFORMATION *info )
{
    return msync_query_event_obj( obj, info );
}

static NTSTATUS linux_release_mutex_obj( int obj, LONG *prev_count )
{
    return msync_release_mutex_obj( obj, prev_count );
}

static NTSTATUS linux_query_mutex_obj( int obj, MUTANT_BASIC_INFORMATION *info )
{
    return msync_query_mutex_obj( obj, info );
}

static NTSTATUS linux_wait_objs( int device, DWORD count, const int *objs, WAIT_TYPE type,
                                 int alert_fd, const LARGE_INTEGER *timeout )
{
    return msync_wait_objs( count, objs, type == WaitAny, alert_fd, timeout );
}

