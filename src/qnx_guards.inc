/* SPDX-License-Identifier: MIT
 * SDK attribute locks map to short state critical sections. I/O group
 * callouts also hold a per-file operation lock across callbacks. Descriptor
 * lifecycle controls bypass that lock and defer release after SDK unwinding.
 */
#define GUARD_IO(name, type)                                                                       \
    static int guard_##name(resmgr_context_t *c, type *m, iofunc_ocb_t *o)                         \
    {                                                                                              \
        state_lock();                                                                              \
        int result = request_cancelled() ? _RESMGR_NOREPLY : name(c, m, o);                        \
        if (request_cancelled())                                                                   \
            result = _RESMGR_NOREPLY;                                                              \
        state_unlock();                                                                            \
        return result;                                                                             \
    }
GUARD_IO(io_read, io_read_t)
GUARD_IO(io_write, io_write_t)
GUARD_IO(io_stat, io_stat_t)
GUARD_IO(io_chmod, io_chmod_t)
GUARD_IO(io_chown, io_chown_t)
GUARD_IO(io_utime, io_utime_t)
GUARD_IO(io_space, io_space_t)
GUARD_IO(io_sync, io_sync_t)
GUARD_IO(io_devctl, io_devctl_t)
GUARD_IO(io_mmap, io_mmap_t)
GUARD_IO(io_openfd, io_openfd_t)
#undef GUARD_IO
#define GUARD_CONNECT(name, type, extra_type)                                                      \
    static int guard_##name(resmgr_context_t *c, type *m, iofunc_attr_t *a, extra_type *v)         \
    {                                                                                              \
        state_lock();                                                                              \
        int result = request_cancelled() ? _RESMGR_NOREPLY : name(c, m, a, v);                     \
        if (request_cancelled())                                                                   \
            result = _RESMGR_NOREPLY;                                                              \
        state_unlock();                                                                            \
        return result;                                                                             \
    }
GUARD_CONNECT(io_open, io_open_t, void)
GUARD_CONNECT(io_unlink, io_unlink_t, void)
GUARD_CONNECT(io_mknod, io_mknod_t, void)
GUARD_CONNECT(io_readlink, io_readlink_t, void)
GUARD_CONNECT(io_link, io_link_t, io_link_extra_t)
GUARD_CONNECT(io_rename, io_rename_t, io_rename_extra_t)
GUARD_CONNECT(io_mount, io_mount_t, io_mount_extra_t)
#undef GUARD_CONNECT
#define GUARD_CLOSE(name, type)                                                                    \
    static int guard_##name(resmgr_context_t *c, type *m, iofunc_ocb_t *o)                         \
    {                                                                                              \
        state_lock();                                                                              \
        int result = name(c, m, o);                                                                \
        if (request_cancelled())                                                                   \
            result = _RESMGR_NOREPLY;                                                              \
        state_unlock();                                                                            \
        return result;                                                                             \
    }
GUARD_CLOSE(io_close, void)
/* The SDK calls close_dup outside lock_ocb. It must remain able to close a
 * cancelled descriptor while an older read still owns the attribute lock. */
static int guard_io_close_dup(resmgr_context_t *c, io_close_t *m, iofunc_ocb_t *o)
{
    state_lock();
    int result = io_close_dup(c, m, o);
    if (request_cancelled())
        result = _RESMGR_NOREPLY;
    state_unlock();
    return result;
}
#undef GUARD_CLOSE
int fuse_interrupted(void)
{
    return request_cancelled();
}
