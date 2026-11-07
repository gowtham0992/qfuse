/* SPDX-License-Identifier: MIT
 * Worker contexts outlive thread_pool_destroy()'s return; drain them before
 * destroying dispatch or session state. The state mutex is held only around
 * backend state, never around third-party callbacks. Per-file operation
 * locks serialize same-file I/O; the namespace gate protects path lifetimes.
 */
static pthread_mutex_t pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pool_drained = PTHREAD_COND_INITIALIZER;
static unsigned pool_contexts;
static dispatch_context_t *pool_alloc(dispatch_t *d)
{
    dispatch_context_t *ctp = dispatch_context_alloc(d);
    if (ctp) {
        pthread_mutex_lock(&pool_mutex);
        pool_contexts++;
        pthread_mutex_unlock(&pool_mutex);
    }
    return ctp;
}
static void pool_free(dispatch_context_t *ctp)
{
    dispatch_context_free(ctp);
    pthread_mutex_lock(&pool_mutex);
    pool_contexts--;
    pthread_cond_broadcast(&pool_drained);
    pthread_mutex_unlock(&pool_mutex);
}
static int namespace_mutation(resmgr_context_t *c)
{
    if (pool_serial)
        return 1;
    if (c->msg->type != _IO_CONNECT)
        return 0;
    unsigned subtype = c->msg->connect.subtype;
    return (subtype != _IO_CONNECT_OPEN && subtype != _IO_CONNECT_COMBINE &&
            subtype != _IO_CONNECT_COMBINE_CLOSE && subtype != _IO_CONNECT_READLINK) ||
           (c->msg->connect.ioflag & (O_CREAT | O_TRUNC));
}
static int control_message(resmgr_context_t *c)
{
    if (c->msg->type == _IO_CONNECT || (c->msg->read.i.combine_len & _IO_COMBINE_FLAG))
        return 0;
    switch (c->msg->type) {
    case _IO_DUP:
    case _IO_OPENFD:
    case _IO_CLOSE:
    case _IO_DEVCTL:
    case _IO_MMAP:
    case _IO_FDINFO:
    case _IO_PATHCONF:
        return 1;
    default:
        return 0;
    }
}
static int pool_handler(dispatch_context_t *ctp)
{
    resmgr_context_t *c = &ctp->resmgr_context;
    /* Pulses, including client unblocks, must never wait on backend state. */
    if (c->rcvid <= 0) {
        dispatching++;
        int result = dispatch_handler(ctp);
        dispatching--;
        drain_closed_ocbs();
        return result;
    }
    int control = control_message(c);
    struct pending_request r = {.rcvid = c->rcvid, .context = c, .payload = !control};
    atomic_init(&r.cancelled, 0);
    pthread_mutex_lock(&pending_mutex);
    if (r.payload && payload_requests == REQUEST_LIMIT) {
        pthread_mutex_unlock(&pending_mutex);
        MsgError(c->rcvid, EAGAIN);
        return 0;
    }
    payload_requests += r.payload;
    r.next = pending;
    pending = &r;
    pthread_mutex_unlock(&pending_mutex);
    current_request = &r;
    int e = 0;
    if (!control && c->msg->type == _IO_CONNECT)
        e = namespace_connect(c);
    int result;
    if (e || request_cancelled()) {
        if (!request_cancelled())
            MsgError(c->rcvid, e);
        namespace_leave();
        result = 0;
    } else {
        dispatching++;
        result = dispatch_handler(ctp);
        dispatching--;
    }
    pthread_mutex_lock(&pending_mutex);
    struct pending_request **link = &pending;
    while (*link && *link != &r)
        link = &(*link)->next;
    if (*link)
        *link = r.next;
    payload_requests -= r.payload;
    pthread_mutex_unlock(&pending_mutex);
    current_request = NULL;
    drain_closed_ocbs();
    namespace_leave();
    return result;
}
/* A receiver-independent fallback handles kernel unblock flags even when all
 * dispatch workers are inside foreign callbacks or waiting for per-file locks.
 * It never takes filesystem locks, invokes callbacks, or consumes SDK pulses. */
static _Atomic int cancel_monitor_stop;
static void *cancel_monitor(void *unused)
{
    (void)unused;
    while (!atomic_load(&cancel_monitor_stop)) {
        pthread_mutex_lock(&pending_mutex);
        for (struct pending_request *r = pending; r; r = r->next) {
            struct _msg_info info;
            if (r->waiting && !atomic_load(&r->cancelled) && MsgInfo(r->rcvid, &info) != -1 &&
                (info.flags & _NTO_MI_UNBLOCK_REQ)) {
                r->interrupt_requested = 1;
                cancel_waiter(r);
            }
        }
        pthread_mutex_unlock(&pending_mutex);
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
    }
    return NULL;
}
static int serve_pool(unsigned workers)
{
    pthread_once(&state_once, init_state_mutex);
    if (state_error)
        return -state_error;
    pool_serial = workers == 1;
    pthread_mutex_lock(&pending_mutex);
    cancelled_requests = 0;
    pthread_mutex_unlock(&pending_mutex);
    thread_pool_attr_t a = {0};
    a.handle = server.dispatch;
    a.block_func = dispatch_block;
    a.unblock_func = dispatch_unblock;
    a.handler_func = pool_handler;
    a.context_alloc = pool_alloc;
    a.context_free = pool_free;
    a.lo_water = workers < 2 ? 2 : workers;
    a.hi_water = a.lo_water;
    a.maximum = 64;
    a.increment = 1;
    thread_pool_t *pool = thread_pool_create(&a, 0);
    if (!pool)
        return -errno;
    pthread_t monitor, releaser;
    release_stop = 0;
    atomic_store(&cancel_monitor_stop, 0);
    int monitor_error = pthread_create(&monitor, NULL, cancel_monitor, NULL);
    if (monitor_error) {
        thread_pool_destroy(pool);
        return -monitor_error;
    }
    int release_error = pthread_create(&releaser, NULL, release_worker, NULL);
    if (release_error) {
        atomic_store(&cancel_monitor_stop, 1);
        pthread_join(monitor, NULL);
        thread_pool_destroy(pool);
        return -release_error;
    }
    if (thread_pool_start(pool)) {
        int e = errno;
        thread_pool_destroy(pool);
        stop_release_worker(releaser);
        atomic_store(&cancel_monitor_stop, 1);
        pthread_join(monitor, NULL);
        return -e;
    }
    atomic_store(&admin_phase, QFUSE_ADMIN_SERVING);
    int signal_number;
    int e = sigwait(&server.signals, &signal_number);
    if (!e)
        server.owner->session.interrupted = signal_number;
    atomic_store(&admin_phase, QFUSE_ADMIN_STOPPING);
    atomic_store(&server.stopping, 1);
    int destroyed = thread_pool_destroy(pool);
    int saved = errno;
    pthread_mutex_lock(&pool_mutex);
    while (pool_contexts)
        pthread_cond_wait(&pool_drained, &pool_mutex);
    pthread_mutex_unlock(&pool_mutex);
    stop_release_worker(releaser);
    atomic_store(&cancel_monitor_stop, 1);
    pthread_join(monitor, NULL);
    if (e)
        return -e;
    return destroyed ? -saved : 0;
}
