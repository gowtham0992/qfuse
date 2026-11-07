/* SPDX-License-Identifier: MIT
 * Request lifetime is independent of client wait lifetime. Never cancel a
 * foreign callback thread: it may own locks in third-party code.
 */
static _Thread_local int dispatching;
static _Thread_local struct open_file *deferred_ocbs;
static void drain_closed_ocbs(void);
static void *release_worker(void *);
static unsigned pool_serial;
static _Thread_local unsigned namespace_depth;
static _Thread_local int namespace_exclusive;
static _Thread_local struct node *operation_owner;
#define REQUEST_LIMIT 32
#define OCB_LIMIT 1024
static unsigned payload_requests, cancelled_requests;
static pthread_mutex_t state_mutex;
static pthread_once_t state_once = PTHREAD_ONCE_INIT;
static int state_error;
static _Thread_local unsigned state_depth;
static pthread_mutex_t pending_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct qf_path_gate namespace_gate;
static _Thread_local struct qf_path_lease namespace_lease;
struct pending_request {
    rcvid_t rcvid;
    resmgr_context_t *context;
    int waiting, interrupt_requested, payload, close_wait;
    struct open_handle *handle;
    _Atomic int cancelled;
    struct pending_request *next;
};
static struct pending_request *pending;
static _Thread_local struct pending_request *current_request;
static _Thread_local unsigned callback_depth;
static int init_recursive_mutex(pthread_mutex_t *mutex)
{
    pthread_mutexattr_t attr;
    int e = pthread_mutexattr_init(&attr);
    if (!e) {
        e = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        if (!e)
            e = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
        if (!e)
            e = pthread_mutex_init(mutex, &attr);
        pthread_mutexattr_destroy(&attr);
    }
    return e;
}
static void init_state_mutex(void)
{
    state_error = init_recursive_mutex(&state_mutex);
    if (!state_error)
        state_error = qf_gate_init(&namespace_gate);
}
static void state_lock(void)
{
    pthread_once(&state_once, init_state_mutex);
    if (state_error || pthread_mutex_lock(&state_mutex))
        abort();
    state_depth++;
}
static void state_unlock(void)
{
    if (!state_depth)
        abort();
    state_depth--;
    if (pthread_mutex_unlock(&state_mutex))
        abort();
}
static unsigned state_drop(void)
{
    unsigned depth = state_depth;
    while (state_depth)
        state_unlock();
    return depth;
}
static void state_restore(unsigned depth)
{
    while (depth--)
        state_lock();
}
static int request_cancelled(void)
{
    return current_request && atomic_load(&current_request->cancelled);
}
static void request_handle(struct open_handle *handle)
{
    if (!current_request)
        return;
    pthread_mutex_lock(&pending_mutex);
    current_request->handle = handle;
    if (atomic_load(&current_request->cancelled))
        atomic_store(&handle->interrupted, 1);
    pthread_mutex_unlock(&pending_mutex);
}
static void namespace_changed(void)
{
    pthread_mutex_lock(&release_mutex);
    release_epoch++;
    pthread_cond_signal(&release_ready);
    pthread_mutex_unlock(&release_mutex);
}
static void cancel_waiter(struct pending_request *r)
{
    if (!atomic_load(&r->cancelled)) {
        atomic_store(&r->cancelled, 1);
        if (r->handle)
            atomic_store(&r->handle->interrupted, 1);
        cancelled_requests++;
        qf_gate_notify(&namespace_gate);
        namespace_changed();
        /* -1 asks the kernel to select EINTR or ETIMEDOUT for this unblock. */
        MsgError(r->rcvid, -1);
    }
}
static void waiting_begin(void)
{
    if (!current_request)
        return;
    pthread_mutex_lock(&pending_mutex);
    current_request->waiting = 1;
    struct _msg_info info;
    if (!atomic_load(&current_request->cancelled) && MsgInfo(current_request->rcvid, &info) != -1 &&
        (info.flags & _NTO_MI_UNBLOCK_REQ))
        current_request->interrupt_requested = 1;
    if (current_request->interrupt_requested)
        cancel_waiter(current_request);
    pthread_mutex_unlock(&pending_mutex);
}
static void waiting_end(void)
{
    if (!current_request)
        return;
    pthread_mutex_lock(&pending_mutex);
    current_request->waiting = callback_depth != 0;
    /* rcvid values may be reused by a later send from the same client.
     * Invalidate this worker's context before any late SDK automatic reply.
     * The SDK retains its OCB binding while this handler is still running. */
    if (atomic_load(&current_request->cancelled))
        current_request->context->rcvid = -1;
    pthread_mutex_unlock(&pending_mutex);
}
static int claim_payload(void)
{
    if (!current_request || current_request->payload)
        return 1;
    pthread_mutex_lock(&pending_mutex);
    int available = payload_requests < REQUEST_LIMIT;
    if (available) {
        payload_requests++;
        current_request->payload = 1;
    }
    pthread_mutex_unlock(&pending_mutex);
    return available;
}
static int namespace_cancelled(void *p)
{
    struct pending_request *r = p;
    return r && (atomic_load(&r->cancelled) ||
                 (r->close_wait && r->handle && atomic_load(&r->handle->interrupted)));
}
static int namespace_enter(const char *const *paths, unsigned count, int exclusive, int wait)
{
    if (namespace_depth)
        return 0;
    const char *serial[] = {"/"};
    if (pool_serial) {
        paths = serial;
        count = 1;
        exclusive = 1;
    }
    waiting_begin();
    unsigned depth = wait ? state_drop() : 0;
    int e = qf_gate_enter(&namespace_gate, &namespace_lease, paths, count, exclusive,
                          namespace_cancelled, current_request, wait);
    if (wait)
        state_restore(depth);
    waiting_end();
    if (wait && e)
        namespace_changed();
    if (!e) {
        namespace_depth = 1;
        namespace_exclusive = exclusive;
    }
    return e;
}
static void namespace_leave(void)
{
    if (!namespace_depth)
        return;
    namespace_depth = 0;
    namespace_exclusive = 0;
    qf_gate_leave(&namespace_gate, &namespace_lease);
    namespace_changed();
}
/* Node paths may move while a request sleeps behind rename. Physical OCB
 * references pin the node; copy its path under state, acquire the lease, then
 * revalidate before any callback sees that path. */
static int namespace_node(struct node *n, int exclusive, int wait)
{
    if (namespace_depth)
        return 0;
    for (;;) {
        state_lock();
        char path[4097];
        strcpy(path, n->path);
        const char *paths[] = {path};
        int mode = exclusive < 0 ? n->hidden && n->physical_ocbs == 1 : exclusive;
        int e = namespace_enter(paths, 1, mode, wait);
        if (e) {
            state_unlock();
            return e;
        }
        int same = !strcmp(path, n->path) &&
                   (exclusive >= 0 || mode == (n->hidden && n->physical_ocbs == 1));
        state_unlock();
        if (same)
            return 0;
        namespace_leave();
        if (!wait)
            return EAGAIN;
    }
}
static int namespace_connect(resmgr_context_t *);
static int borrow_namespace(void)
{
    const char *paths[] = {"/"};
    return !namespace_enter(paths, 1, 0, 0);
}
static int callback_pause(unsigned *depth)
{
    /* Control messages cannot smuggle a foreign callback past admission. */
    if (current_request && (!claim_payload() || !borrow_namespace()))
        return -EAGAIN;
    callback_depth++;
    waiting_begin();
    *depth = state_drop();
    return 0;
}
static void callback_resume(unsigned depth)
{
    state_restore(depth);
    callback_depth--;
    waiting_end();
}
static int unblock_request(resmgr_context_t *ctp)
{
    struct _msg_info info;
    pthread_mutex_lock(&pending_mutex);
    if (MsgInfo(ctp->rcvid, &info) != -1 && (info.flags & _NTO_MI_UNBLOCK_REQ)) {
        for (struct pending_request *r = pending; r; r = r->next) {
            if (r->rcvid == ctp->rcvid && !atomic_load(&r->cancelled)) {
                r->interrupt_requested = 1;
                if (r->waiting)
                    cancel_waiter(r);
                break;
            }
        }
    }
    pthread_mutex_unlock(&pending_mutex);
    return _RESMGR_NOREPLY;
}
static int unblock_io(resmgr_context_t *c, io_pulse_t *m, iofunc_ocb_t *o)
{
    (void)m;
    (void)o;
    return unblock_request(c);
}
static int unblock_connect(resmgr_context_t *c, io_pulse_t *m, iofunc_attr_t *a, void *v)
{
    (void)m;
    (void)a;
    (void)v;
    return unblock_request(c);
}
static int attr_state_lock(iofunc_attr_t *attr)
{
    (void)attr;
    state_lock();
    return EOK;
}
static int attr_state_unlock(iofunc_attr_t *attr)
{
    (void)attr;
    state_unlock();
    return EOK;
}
static int attr_state_trylock(iofunc_attr_t *attr)
{
    (void)attr;
    pthread_once(&state_once, init_state_mutex);
    if (state_error)
        return state_error;
    int e = pthread_mutex_trylock(&state_mutex);
    if (!e)
        state_depth++;
    return e;
}
/* A kernel wait retains the mutex's priority-inheritance protocol. Periodic
 * monotonic deadlines let an unblocked client abandon a lock it never owned;
 * its dispatcher can then unwind and return the admission slot. Never unlock
 * a mutex on behalf of a thread already executing a foreign callback. */
static int operation_lock(pthread_mutex_t *mutex)
{
    if (!current_request)
        return pthread_mutex_lock(mutex);
    if (request_cancelled())
        return ECANCELED;
    int e = pthread_mutex_trylock(mutex);
    while (e == EBUSY || e == ETIMEDOUT) {
        if (request_cancelled())
            return ECANCELED;
        struct timespec deadline;
        if (clock_gettime(CLOCK_MONOTONIC, &deadline))
            return errno;
        deadline.tv_nsec += 10000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
        e = pthread_mutex_clocklock(mutex, CLOCK_MONOTONIC, &deadline);
    }
    if (!e && request_cancelled()) {
        pthread_mutex_unlock(mutex);
        return ECANCELED;
    }
    return e;
}
static int lock_ocb(resmgr_context_t *c, void *v, iofunc_ocb_t *o)
{
    request_handle(((struct open_file *)o)->handle);
    waiting_begin();
    struct node *n = (struct node *)o->attr;
    int e = 0;
    if (current_request && current_request->payload) {
        e = namespace_node(n, 0, 1);
        waiting_begin();
        if (!e)
            e = operation_lock(&n->operation_mutex);
        if (!e)
            operation_owner = n;
    }
    if (!e) {
        e = iofunc_lock_ocb_default(c, v, o);
        if (e && operation_owner) {
            pthread_mutex_unlock(&n->operation_mutex);
            operation_owner = NULL;
        }
    }
    if (!e)
        state_lock();
    waiting_end();
    return e;
}
static int unlock_ocb(resmgr_context_t *c, void *v, iofunc_ocb_t *o)
{
    int e = iofunc_unlock_ocb_default(c, v, o);
    if (operation_owner) {
        pthread_mutex_unlock(&operation_owner->operation_mutex);
        operation_owner = NULL;
    }
    state_unlock();
    return e;
}
static int guarded_reply(resmgr_context_t *c, void *data, size_t size)
{
    return request_cancelled() ? 0 : resmgr_msgreply(c, data, size);
}
static int guarded_replyv(resmgr_context_t *c, const iov_t *iov, int parts)
{
    return request_cancelled() ? 0 : resmgr_msgreplyv(c, iov, parts);
}
/* SDK calls above retain their native declarations; only backend replies use
 * these guards. Cancellation is only issued while a request is waiting. */
#define resmgr_msgreply guarded_reply
#define resmgr_msgreplyv guarded_replyv
