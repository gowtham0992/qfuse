/* SPDX-License-Identifier: MIT
 * Native request validation and writable high-level callback dispatch.
 * Included by qnx_backend.c so the node/OCB implementation stays private.
 */
static int redirect_parent(resmgr_context_t *, io_open_t *, char *, struct qf_request *,
                           struct _client_info *, int);

static int connect_path(resmgr_context_t *ctp, const struct _io_connect *msg, char path[4097])
{
    size_t len = msg->path_len;
    if (!len || len > 4096)
        return ENAMETOOLONG;
    path[0] = '/';
    if (resmgr_msgget(ctp, path + 1, len, offsetof(struct _io_connect, path)) != (ssize_t)len)
        return EINVAL;
    if (path[len] || strnlen(path + 1, len) != len - 1)
        return EINVAL;
    return 0;
}
static int check_parents(resmgr_context_t *ctp, struct qf_request *r, char *path,
                         struct _client_info *info, int modify)
{
    for (char *p = path;; p++) {
        if (*p && *p != '/')
            continue;
        char saved = *p;
        *p = 0;
        if ((p == path && path[1]) || (p != path && saved == '/')) {
            struct stat st;
            int result = qf_getattr(r, p == path ? "/" : path, &st, NULL);
            if (result < 0) {
                *p = saved;
                return -result;
            }
            if (!S_ISDIR(st.st_mode)) {
                *p = saved;
                return ENOTDIR;
            }
            iofunc_attr_t a;
            iofunc_attr_init(&a, st.st_mode, NULL, NULL);
            update_attr(&a, &st);
            int access = S_IEXEC;
            if (modify && !strchr(p + 1, '/'))
                access |= S_IWRITE;
            int error = iofunc_check_access(ctp, &a, access, info);
            *p = saved;
            if (error)
                return error;
        } else
            *p = saved;
        if (!saved)
            break;
    }
    return 0;
}
static int creation_owner(struct qf_request *r, const char *path, struct fuse_file_info *fi,
                          struct _client_info *info)
{
    /* The daemon is privileged; passthrough callbacks otherwise create as root.
       Root callers may intentionally use a remote account with different IDs. */
    if (info->cred.euid == geteuid())
        return 0;
    char parent[4097];
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (slash == parent)
        parent[1] = 0;
    else
        *slash = 0;
    struct stat st, p;
    int result = qf_getattr(r, parent, &p, NULL);
    if (result < 0)
        return -result;
    result = qf_getattr(r, path, &st, fi);
    if (result < 0)
        return -result;
    gid_t gid = (p.st_mode & S_ISGID) ? p.st_gid : info->cred.egid;
    if (st.st_uid == info->cred.euid && st.st_gid == gid)
        return 0;
    result = qf_chown(r, path, info->cred.euid, gid, fi);
    return result < 0 ? -result : 0;
}
static int writable(void)
{
    return server.owner->readonly ? EROFS : 0;
}
static int refresh(struct qf_request *r, struct open_file *f)
{
    struct node *n = (struct node *)f->base.attr;
    struct stat st;
    int status = qf_getattr(r, n->path, &st, !S_ISREG(n->attr.mode) ? NULL : &f->handle->fi);
    if (!status)
        update_attr(&n->attr, &st);
    return status < 0 ? -status : 0;
}
static int io_write(resmgr_context_t *ctp, io_write_t *msg, iofunc_ocb_t *base)
{
    int e = writable();
    if (e)
        return e;
    e = iofunc_write_verify(ctp, msg, base, NULL);
    if (e)
        return e;
    if (!S_ISREG(base->attr->mode))
        return S_ISDIR(base->attr->mode) ? EISDIR : EINVAL;
    unsigned xtype = msg->i.xtype & _IO_XTYPE_MASK;
    off_t off = base->offset;
    size_t header = sizeof(msg->i);
    if (xtype == _IO_XTYPE_OFFSET) {
        struct _xtype_offset x;
        if (resmgr_msgget(ctp, &x, sizeof(x), header) != sizeof(x))
            return EINVAL;
        off = x.offset;
        header += sizeof(x);
    } else if (xtype != _IO_XTYPE_NONE)
        return ENOSYS;
    if (off < 0)
        return EINVAL;
    size_t count = _IO_WRITE_GET_NBYTES(msg);
    if (count > READ_LIMIT)
        count = READ_LIMIT;
    struct qf_request r;
    struct _client_info *info = NULL;
    e = request(ctp, base->ioflag, &r, &info);
    if (e)
        return e;
    struct open_file *f = (struct open_file *)base;
    struct node *n = (struct node *)base->attr;
    if ((base->ioflag & O_APPEND) && xtype == _IO_XTYPE_NONE) {
        e = refresh(&r, f);
        if (e)
            goto done;
        off = base->attr->nbytes;
    }
    if (off > INT64_MAX - (off_t)count) {
        e = EOVERFLOW;
        goto done;
    }
    char *data = malloc(count ? count : 1);
    if (!data) {
        e = ENOMEM;
        goto done;
    }
    if (resmgr_msgget(ctp, data, count, header) != (ssize_t)count) {
        free(data);
        e = EINVAL;
        goto done;
    }
    int result = qf_write(&r, n->path, data, count, off, &f->handle->fi);
    free(data);
    if (result < 0) {
        e = -result;
        goto done;
    }
    if (xtype == _IO_XTYPE_NONE)
        base->offset = off + result;
    if (off + result > base->attr->nbytes)
        base->attr->nbytes = off + result;
    _IO_SET_WRITE_NBYTES(ctp, result);
    e = EOK;
done:
    iofunc_client_info_ext_free(&info);
    return e;
}
static int io_chmod(resmgr_context_t *ctp, io_chmod_t *msg, iofunc_ocb_t *base)
{
    int e = writable();
    if (e)
        return e;
    iofunc_attr_t copy = *base->attr;
    e = iofunc_chmod(ctp, msg, base, &copy);
    if (e)
        return e;
    struct qf_request r;
    struct _client_info *info = NULL;
    e = request(ctp, base->ioflag, &r, &info);
    if (e)
        return e;
    struct open_file *f = (struct open_file *)base;
    struct node *n = (struct node *)base->attr;
    int result =
        qf_chmod(&r, n->path, msg->i.mode, !S_ISREG(base->attr->mode) ? NULL : &f->handle->fi);
    if (result < 0)
        e = -result;
    else
        e = refresh(&r, f);
    iofunc_client_info_ext_free(&info);
    return e;
}
static int io_chown(resmgr_context_t *ctp, io_chown_t *msg, iofunc_ocb_t *base)
{
    int e = writable();
    if (e)
        return e;
    iofunc_attr_t copy = *base->attr;
    e = iofunc_chown(ctp, msg, base, &copy);
    if (e)
        return e;
    struct qf_request r;
    struct _client_info *info = NULL;
    e = request(ctp, base->ioflag, &r, &info);
    if (e)
        return e;
    struct open_file *f = (struct open_file *)base;
    int result = qf_chown(&r, ((struct node *)base->attr)->path, msg->i.uid, msg->i.gid,
                          !S_ISREG(base->attr->mode) ? NULL : &f->handle->fi);
    if (result < 0)
        e = -result;
    else
        e = refresh(&r, f);
    iofunc_client_info_ext_free(&info);
    return e;
}
static int io_utime(resmgr_context_t *ctp, io_utime_t *msg, iofunc_ocb_t *base)
{
    int e = writable();
    if (e)
        return e;
    iofunc_attr_t copy = *base->attr;
    e = iofunc_utime(ctp, msg, base, &copy);
    if (e)
        return e;
    struct timespec times[2];
    if (msg->i.type == _IO_UTIME64) {
        times[0] = (struct timespec){msg->i64.atime_s, msg->i64.atime_ns};
        times[1] = (struct timespec){msg->i64.mtime_s, msg->i64.mtime_ns};
    } else if (msg->i.cur_flag) {
        times[0].tv_sec = times[1].tv_sec = 0;
        times[0].tv_nsec = times[1].tv_nsec = UTIME_NOW;
    } else {
        times[0] = (struct timespec){msg->i.times._actime, 0};
        times[1] = (struct timespec){msg->i.times._modtime, 0};
    }
    struct qf_request r;
    struct _client_info *info = NULL;
    e = request(ctp, base->ioflag, &r, &info);
    if (e)
        return e;
    struct open_file *f = (struct open_file *)base;
    int result = qf_utimens(&r, ((struct node *)base->attr)->path, times,
                            !S_ISREG(base->attr->mode) ? NULL : &f->handle->fi);
    if (result < 0)
        e = -result;
    else
        e = refresh(&r, f);
    iofunc_client_info_ext_free(&info);
    return e;
}
static int io_space(resmgr_context_t *ctp, io_space_t *msg, iofunc_ocb_t *base)
{
    int e = writable();
    if (e)
        return e;
    e = iofunc_space_verify(ctp, msg, base, NULL);
    if (e)
        return e;
    if (!S_ISREG(base->attr->mode))
        return S_ISDIR(base->attr->mode) ? EISDIR : EINVAL;
    if (msg->i.subtype != F_FREESP || msg->i.len)
        return ENOTSUP;
    if (msg->i.start > INT64_MAX)
        return EINVAL;
    off_t size = msg->i.start, origin = 0;
    if (msg->i.whence == SEEK_CUR)
        origin = base->offset;
    else if (msg->i.whence == SEEK_END)
        origin = base->attr->nbytes;
    else if (msg->i.whence != SEEK_SET)
        return EINVAL;
    if (origin < 0 || origin > INT64_MAX - size)
        return EOVERFLOW;
    size += origin;
    struct qf_request r;
    struct _client_info *info = NULL;
    e = request(ctp, base->ioflag, &r, &info);
    if (e)
        return e;
    struct open_file *f = (struct open_file *)base;
    int result = qf_truncate(&r, ((struct node *)base->attr)->path, size, &f->handle->fi);
    if (result < 0)
        e = -result;
    else {
        base->attr->nbytes = size;
        msg->o = size;
        e = _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
    }
    iofunc_client_info_ext_free(&info);
    return e;
}
static int io_sync(resmgr_context_t *ctp, io_sync_t *msg, iofunc_ocb_t *base)
{
    if (!S_ISREG(base->attr->mode))
        return EINVAL;
    int e = iofunc_sync_verify(ctp, msg, base);
    if (e)
        return e;
    struct qf_request r;
    struct _client_info *info = NULL;
    e = request(ctp, base->ioflag, &r, &info);
    if (e)
        return e;
    int result =
        qf_fsync(&r, ((struct node *)base->attr)->path, 0, &((struct open_file *)base)->handle->fi);
    iofunc_client_info_ext_free(&info);
    return result < 0 ? -result : 0;
}
static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, iofunc_ocb_t *base)
{
    if (trace_messages)
        fprintf(stderr, "QFuse devctl cmd=%x own=%x size=%u\n", msg->i.dcmd,
                (unsigned)QFUSE_CONTROL_GET_INFO, msg->i.nbytes);
    if (msg->i.dcmd == QFUSE_CONTROL_GET_INFO) {
        struct qfuse_mount_info info = {.magic = QFUSE_CONTROL_MAGIC, .version = 1};
        strcpy(info.mountpoint, server.owner->mountpoint);
        memset(&msg->o, 0, sizeof(msg->o));
        msg->o.nbytes = sizeof(info);
        iov_t iov[2];
        SETIOV(&iov[0], &msg->o, sizeof(msg->o));
        SETIOV(&iov[1], &info, sizeof(info));
        resmgr_msgreplyv(ctp, iov, 2);
        return _RESMGR_NOREPLY;
    }
    if (msg->i.dcmd != DCMD_FSYS_STATVFS)
        return iofunc_devctl_default(ctp, msg, base);
    if (!namespace_depth &&
        (!claim_payload() || namespace_node((struct node *)base->attr, 0, 0)))
        return EAGAIN;
    struct qf_request r;
    struct _client_info *info = NULL;
    int e = request(ctp, base->ioflag, &r, &info);
    if (e)
        return e;
    struct statvfs st;
    int result = qf_statfs(&r, ((struct node *)base->attr)->path, &st);
    iofunc_client_info_ext_free(&info);
    if (result < 0)
        return -result;
    /* Intrinsic identity belongs to this virtual mount, not its backing FS.
       FUSE's statfs contract ignores the callback's fsid and mount flags. */
    st.f_fsid = server.mount.dev;
    snprintf(st.f_basetype, sizeof(st.f_basetype), "qfuse");
    struct __msg_statvfs out;
    __msg_statvfs_init(&out, &st);
    out.f_flag |= server.mount.flags & (ST_RDONLY | ST_NOSUID);
    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.nbytes = sizeof(out);
    iov_t iov[2];
    SETIOV(&iov[0], &msg->o, sizeof(msg->o));
    SETIOV(&iov[1], &out, sizeof(out));
    resmgr_msgreplyv(ctp, iov, 2);
    return _RESMGR_NOREPLY;
}
static int io_mknod(resmgr_context_t *ctp, io_mknod_t *msg, iofunc_attr_t *handle, void *extra)
{
    (void)handle;
    (void)extra;
    int e = writable();
    if (e)
        return e;
    char path[4097];
    e = connect_path(ctp, &msg->connect, path);
    if (e)
        return e;
    struct qf_request r;
    struct _client_info *info = NULL;
    e = request(ctp, msg->connect.ioflag, &r, &info);
    if (e)
        return e;
    e = redirect_parent(ctp, (io_open_t *)msg, path, &r, info, 1);
    if (e)
        goto done;
    struct stat st;
    int result = qf_getattr(&r, path, &st, NULL);
    if (!result) {
        e = EEXIST;
        goto done;
    }
    if (result != -ENOENT) {
        e = -result;
        goto done;
    }
    if (!S_ISDIR(msg->connect.mode)) {
        e = ENOTSUP;
        goto done;
    }
    result = qf_mkdir(&r, path, msg->connect.mode & 07777);
    e = result < 0 ? -result : 0;
    if (!e) {
        e = creation_owner(&r, path, NULL, info);
        if (e)
            qf_rmdir(&r, path);
    }
done:
    iofunc_client_info_ext_free(&info);
    return e;
}
static int sticky_check(resmgr_context_t *ctp, struct qf_request *r, char *path,
                        struct _client_info *info)
{
    char parent[4097];
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (slash == parent)
        parent[1] = 0;
    else
        *slash = 0;
    struct stat p, st;
    int result = qf_getattr(r, parent, &p, NULL);
    if (result < 0)
        return -result;
    result = qf_getattr(r, path, &st, NULL);
    if (result < 0)
        return -result;
    (void)ctp;
    if ((p.st_mode & S_ISVTX) && info->cred.euid != 0 && info->cred.euid != p.st_uid &&
        info->cred.euid != st.st_uid)
        return EPERM;
    return 0;
}
static void rehash_nodes(void)
{
    memset(server.buckets, 0, sizeof(server.buckets));
    for (struct node *n = server.nodes; n; n = n->next) {
        unsigned h = 2166136261u;
        for (const unsigned char *p = (const unsigned char *)n->path; *p; p++)
            h = (h ^ *p) * 16777619u;
        h %= NODE_BUCKETS;
        n->hash_next = server.buckets[h];
        server.buckets[h] = n;
    }
}
static int owned_hidden(const char *path)
{
    if (!server.hidden_count)
        return 0;
    for (struct node *n = server.nodes; n; n = n->next)
        if (n->hidden && !strcmp(n->path, path))
            return 1;
    return 0;
}
struct hidden_change {
    struct node *node;
    char *old_path;
};
static int hide_open(struct qf_request *r, const char *path, struct hidden_change *change)
{
    memset(change, 0, sizeof(*change));
    struct node *node = NULL;
    for (struct node *n = server.nodes; n; n = n->next)
        if (!n->retired && !n->hidden && n->opens && !strcmp(n->path, path)) {
            node = n;
            break;
        }
    if (!node || !S_ISREG(node->attr.mode))
        return 0;
    char *newpath = malloc(4097);
    if (!newpath)
        return ENOMEM;
    size_t parent = (size_t)(strrchr(path, '/') - path) + 1;
    int status = -EEXIST;
    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        memcpy(newpath, path, parent);
        int n = snprintf(newpath + parent, 4097 - parent, ".qfuse-hidden.%ld.%u", (long)getpid(),
                         ++server.hidden_sequence);
        if (n < 0 || (size_t)n >= 4097 - parent) {
            free(newpath);
            return ENAMETOOLONG;
        }
        unsigned previous_scopes = namespace_lease.count;
        qf_gate_unextend(&namespace_gate, &namespace_lease);
        if (previous_scopes != namespace_lease.count)
            namespace_changed();
        int reservation = qf_gate_extend(&namespace_gate, &namespace_lease, newpath);
        if (reservation == EBUSY)
            continue;
        if (reservation) {
            free(newpath);
            return reservation;
        }
        struct stat st;
        status = qf_getattr(r, newpath, &st, NULL);
        if (status == -ENOENT)
            break;
        if (status < 0) {
            free(newpath);
            return -status;
        }
    }
    if (status != -ENOENT) {
        free(newpath);
        return EEXIST;
    }
    status = qf_rename(r, path, newpath);
    if (status < 0) {
        free(newpath);
        return -status;
    }
    change->node = node;
    change->old_path = node->path;
    node->path = newpath;
    node->hidden = 1;
    node->attr.nlink = 0;
    server.hidden_count++;
    rehash_nodes();
    return 0;
}
static int undo_hidden(struct qf_request *r, struct hidden_change *change)
{
    if (!change->node)
        return 0;
    int result = qf_rename(r, change->node->path, change->old_path);
    if (result < 0) {
        free(change->old_path);
        change->old_path = NULL;
        return EIO;
    }
    free(change->node->path);
    change->node->path = change->old_path;
    change->old_path = NULL;
    change->node->hidden = 0;
    server.hidden_count--;
    rehash_nodes();
    return 0;
}
static void retire_path(const char *path)
{
    for (struct node *n = server.nodes; n; n = n->next)
        if (!strcmp(n->path, path))
            n->retired = 1;
}
static int io_unlink(resmgr_context_t *ctp, io_unlink_t *msg, iofunc_attr_t *handle, void *extra)
{
    (void)handle;
    (void)extra;
    int e = writable();
    if (e)
        return e;
    char path[4097];
    e = connect_path(ctp, &msg->connect, path);
    if (e)
        return e;
    if (!strcmp(path, "/"))
        return EBUSY;
    if (owned_hidden(path))
        return ENOENT;
    struct qf_request r;
    struct _client_info *info = NULL;
    e = request(ctp, msg->connect.ioflag, &r, &info);
    if (e)
        return e;
    e = redirect_parent(ctp, (io_open_t *)msg, path, &r, info, 1);
    if (e)
        goto done;
    e = sticky_check(ctp, &r, path, info);
    if (e)
        goto done;
    struct stat st;
    int result = qf_getattr(&r, path, &st, NULL);
    if (result < 0) {
        e = -result;
        goto done;
    }
    int directory = !!(msg->connect.mode & S_IFDIR);
    if (directory && !S_ISDIR(st.st_mode)) {
        e = ENOTDIR;
        goto done;
    }
    if (!directory && S_ISDIR(st.st_mode)) {
        e = EISDIR;
        goto done;
    }
    struct hidden_change hidden = {0};
    if (!directory) {
        e = hide_open(&r, path, &hidden);
        if (e)
            goto done;
    }
    if (hidden.node) {
        free(hidden.old_path);
        retire_path(path);
        e = 0;
        goto done;
    }
    result = directory ? qf_rmdir(&r, path) : qf_unlink(&r, path);
    e = result < 0 ? -result : 0;
    if (!e)
        retire_path(path);
done:
    iofunc_client_info_ext_free(&info);
    return e;
}
static int extra_path(resmgr_context_t *ctp, const struct _io_connect *msg, char out[4097],
                      int canonical)
{
    size_t len = msg->extra_len;
    if (!len || len > 4097)
        return EINVAL;
    char raw[4097];
    /* Connect extras start at the aligned end of the pathname. */
    size_t offset = offsetof(struct _io_connect, path) + msg->path_len;
    offset = (offset + _QNX_MSG_ALIGN - 1) & ~(size_t)(_QNX_MSG_ALIGN - 1);
    if (resmgr_msgget(ctp, raw, len, offset) != (ssize_t)len)
        return EINVAL;
    if (raw[len - 1] || strnlen(raw, len) != len - 1)
        return EINVAL;
    if (canonical && raw[0] != '/') {
        if (len > 4096)
            return ENAMETOOLONG;
        out[0] = '/';
        memcpy(out + 1, raw, len);
    } else
        memcpy(out, raw, len);
    return 0;
}
static int io_link(resmgr_context_t *ctp, io_link_t *msg, iofunc_attr_t *handle,
                   io_link_extra_t *extra)
{
    (void)handle;
    (void)extra;
    int e = writable();
    if (e)
        return e;
    if (msg->connect.extra_type != _IO_CONNECT_EXTRA_SYMLINK)
        return ENOTSUP;
    char path[4097], target[4097];
    e = connect_path(ctp, &msg->connect, path);
    if (e)
        return e;
    e = extra_path(ctp, &msg->connect, target, 0);
    if (e)
        return e;
    struct qf_request r;
    struct _client_info *info = NULL;
    e = request(ctp, msg->connect.ioflag, &r, &info);
    if (e)
        return e;
    e = redirect_parent(ctp, (io_open_t *)msg, path, &r, info, 1);
    if (e)
        goto done;
    if (!e) {
        int result = qf_symlink(&r, target, path);
        e = result < 0 ? -result : 0;
    }
done:
    iofunc_client_info_ext_free(&info);
    return e;
}
static int io_rename(resmgr_context_t *ctp, io_rename_t *msg, iofunc_attr_t *handle,
                     io_rename_extra_t *extra)
{
    (void)handle;
    (void)extra;
    int e = writable();
    if (e)
        return e;
    char to[4097], from[4097];
    e = connect_path(ctp, &msg->connect, to);
    if (e)
        return e;
    e = extra_path(ctp, &msg->connect, from, 1);
    if (e)
        return e;
    if (!strcmp(from, "/") || !strcmp(to, "/"))
        return EBUSY;
    if (owned_hidden(from) || owned_hidden(to))
        return ENOENT;
    struct qf_request r;
    struct _client_info *info = NULL;
    e = request(ctp, msg->connect.ioflag, &r, &info);
    if (e)
        return e;
    e = check_parents(ctp, &r, from, info, 1);
    if (!e)
        e = check_parents(ctp, &r, to, info, 1);
    if (!e)
        e = sticky_check(ctp, &r, from, info);
    struct stat exists;
    int status = qf_getattr(&r, to, &exists, NULL);
    if (!e && status == 0)
        e = sticky_check(ctp, &r, to, info);
    if (e)
        goto done;
    if (!strcmp(from, to)) {
        int result = qf_rename(&r, from, to);
        e = result < 0 ? -result : 0;
        goto done;
    }
    /* Allocate all namespace changes before mutating backing storage. */
    size_t len = strlen(from);
    size_t count = 0;
    for (struct node *n = server.nodes; n; n = n->next)
        if (!n->retired && !strncmp(n->path, from, len) && (!n->path[len] || n->path[len] == '/'))
            count++;
    struct node **nodes = calloc(count ? count : 1, sizeof(*nodes));
    char **paths = calloc(count ? count : 1, sizeof(*paths));
    if (!nodes || !paths) {
        free(nodes);
        free(paths);
        e = ENOMEM;
        goto done;
    }
    size_t i = 0;
    for (struct node *n = server.nodes; n; n = n->next)
        if (!n->retired && !strncmp(n->path, from, len) && (!n->path[len] || n->path[len] == '/')) {
            nodes[i] = n;
            size_t size = strlen(to) + strlen(n->path + len) + 1;
            if (size > 4097) {
                e = ENAMETOOLONG;
                break;
            }
            paths[i] = malloc(size);
            if (!paths[i]) {
                e = ENOMEM;
                break;
            }
            snprintf(paths[i], size, "%s%s", to, n->path + len);
            i++;
        }
    struct hidden_change hidden = {0};
    if (!e)
        e = hide_open(&r, to, &hidden);
    if (!e) {
        int result = qf_rename(&r, from, to);
        if (result < 0)
            e = -result;
    }
    if (e && hidden.node) {
        int rollback = undo_hidden(&r, &hidden);
        if (rollback)
            e = rollback;
    }
    if (!e)
        free(hidden.old_path);
    if (!e) {
        retire_path(to);
        for (i = 0; i < count; i++) {
            free(nodes[i]->path);
            nodes[i]->path = paths[i];
            paths[i] = NULL;
        }
    }
    /* Rehash existing stable nodes: open handles keep their node identity. */
    if (!e)
        rehash_nodes();
    for (i = 0; i < count; i++)
        free(paths[i]);
    free(paths);
    free(nodes);
done:
    iofunc_client_info_ext_free(&info);
    return e;
}
static int io_readlink(resmgr_context_t *ctp, io_readlink_t *msg, iofunc_attr_t *handle,
                       void *extra)
{
    (void)handle;
    (void)extra;
    if (trace_messages)
        fprintf(stderr, "QFuse readlink handler subtype=%u reply_max=%u\n", msg->connect.subtype,
                msg->connect.reply_max);
    char path[4097], target[4097];
    int e = connect_path(ctp, &msg->connect, path);
    if (e)
        return e;
    struct qf_request r;
    struct _client_info *info = NULL;
    e = request(ctp, msg->connect.ioflag, &r, &info);
    if (e)
        return e;
    e = redirect_parent(ctp, (io_open_t *)msg, path, &r, info, 0);
    if (e)
        goto done;
    struct stat st;
    if (!e) {
        int result = qf_getattr(&r, path, &st, NULL);
        if (result < 0)
            e = -result;
        else if (!S_ISLNK(st.st_mode))
            e = EINVAL;
    }
    if (!e) {
        int result = qf_readlink(&r, path, target, sizeof(target));
        if (result < 0)
            e = -result;
        else {
            size_t len = strlen(target);
            /* QNX 8 libc consumes a connect-link reply header before the text. */
            struct _io_connect_link_reply reply = {0};
            reply.path_len = len;
            iov_t iov[2];
            SETIOV(&iov[0], &reply, sizeof(reply));
            SETIOV(&iov[1], target, len);
            _RESMGR_STATUS(ctp, len);
            resmgr_msgreplyv(ctp, iov, 2);
            e = _RESMGR_NOREPLY;
        }
    }
done:
    iofunc_client_info_ext_free(&info);
    return e;
}
static int redirect_link(resmgr_context_t *ctp, io_open_t *msg, const char *path,
                         struct qf_request *r, const char *suffix)
{
    if (!suffix && (msg->connect.ioflag & O_NOFOLLOW))
        return ELOOP;
    char target[4097], resolved[8194];
    int result = qf_readlink(r, path, target, sizeof(target));
    if (result < 0)
        return -result;
    if (target[0] == '/')
        result = snprintf(resolved, sizeof(resolved), "%s", target);
    else {
        char parent[4097];
        strcpy(parent, path);
        char *slash = strrchr(parent, '/');
        if (slash == parent)
            parent[1] = 0;
        else
            *slash = 0;
        result = snprintf(resolved, sizeof(resolved), "%s%s%s/%s", server.owner->mountpoint,
                          !strcmp(parent, "/") ? "" : parent, "", target);
    }
    if (result < 0 || result > 4096)
        return ENAMETOOLONG;
    if (suffix) {
        size_t used = strlen(resolved);
        int added = snprintf(resolved + used, sizeof(resolved) - used, "/%s", suffix);
        if (added < 0 || used + (size_t)added > 4096)
            return ENAMETOOLONG;
        result = used + added;
    }
    unsigned eflag = msg->connect.eflag;
    struct _io_connect_link_reply reply = {0};
    reply.eflag = eflag;
    reply.path_len = result + 1;
    _IO_SET_CONNECT_RET(ctp, _IO_CONNECT_RET_LINK);
    iov_t iov[2];
    SETIOV(&iov[0], &reply, sizeof(reply));
    SETIOV(&iov[1], resolved, result + 1);
    resmgr_msgreplyv(ctp, iov, 2);
    return _RESMGR_NOREPLY;
}
static int io_mount(resmgr_context_t *ctp, io_mount_t *msg, iofunc_attr_t *handle,
                    io_mount_extra_t *extra)
{
    (void)handle;
    if (!extra || !(extra->flags & _MOUNT_UNMOUNT))
        return ENOSYS;
    char path[4097];
    int e = connect_path(ctp, &msg->connect, path);
    if (e)
        return e;
    if (strcmp(path, "/"))
        return EINVAL;
    struct qf_request r;
    struct _client_info *info = NULL;
    e = request(ctp, 0, &r, &info);
    if (e)
        return e;
    if (info->cred.euid != 0)
        e = EPERM;
    iofunc_client_info_ext_free(&info);
    if (e)
        return e;
    for (struct node *n = server.nodes; !(extra->flags & _MOUNT_FORCE) && n; n = n->next)
        if (n->opens) {
            if (trace_messages || getenv("QFUSE_TRACE_BUSY"))
                fprintf(stderr, "QFuse busy node=%s opens=%u attr_count=%u\n", n->path, n->opens,
                        n->attr.count);
            return EBUSY;
        }
    /* Reply before requesting loop shutdown, so teardown cannot race the reply. */
    resmgr_msgreply(ctp, NULL, 0);
    fuse_exit(server.owner);
    return _RESMGR_NOREPLY;
}

static int redirect_parent(resmgr_context_t *ctp, io_open_t *msg, char *path, struct qf_request *r,
                           struct _client_info *info, int modify)
{
    struct stat st;
    int result = qf_getattr(r, "/", &st, NULL);
    if (result < 0)
        return -result;
    if (!S_ISDIR(st.st_mode))
        return ENOTDIR;
    iofunc_attr_t attr;
    iofunc_attr_init(&attr, st.st_mode, NULL, NULL);
    update_attr(&attr, &st);
    int e = iofunc_check_access(
        ctp, &attr, S_IEXEC | ((modify && !strchr(path + 1, '/')) ? S_IWRITE : 0), info);
    if (e)
        return e;
    for (char *p = path + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            result = qf_getattr(r, path, &st, NULL);
            if (result < 0) {
                *p = '/';
                return -result;
            }
            if (S_ISLNK(st.st_mode)) {
                e = redirect_link(ctp, msg, path, r, p + 1);
                *p = '/';
                return e;
            }
            if (!S_ISDIR(st.st_mode)) {
                *p = '/';
                return ENOTDIR;
            }
            iofunc_attr_init(&attr, st.st_mode, NULL, NULL);
            update_attr(&attr, &st);
            e = iofunc_check_access(
                ctp, &attr, S_IEXEC | ((modify && !strchr(p + 1, '/')) ? S_IWRITE : 0), info);
            *p = '/';
            if (e)
                return e;
        }
    return 0;
}
static int io_close_dup(resmgr_context_t *ctp, io_close_t *msg, iofunc_ocb_t *base)
{
    int result = 0;
    if (S_ISREG(base->attr->mode) && server.session.ops.flush) {
        struct open_file *f = (struct open_file *)base;
        int take_gate = !namespace_depth;
        request_handle(f->handle);
        int flush_ready = 0;
        if (current_request && !atomic_load(&f->handle->interrupted) && claim_payload()) {
            current_request->close_wait = pool_serial;
            flush_ready = !take_gate || !namespace_node(f->node, 0, pool_serial);
            current_request->close_wait = 0;
            if (atomic_load(&f->handle->interrupted))
                flush_ready = 0;
        }
        if (!current_request || !flush_ready) {
            f->handle->deferred_flush = 1;
            if (current_request)
                result = -EAGAIN;
            if (take_gate)
                namespace_leave();
        } else {
            struct qf_request r = f->opener;
            struct _client_info *info = NULL;
            (void)request(ctp, base->ioflag, &r, &info);
            result = qf_flush(&r, f->node->path, &f->handle->fi);
            if (!result)
                f->handle->deferred_flush = 0;
            if (info)
                iofunc_client_info_ext_free(&info);
            if (take_gate)
                namespace_leave();
        }
    }
    int e = iofunc_close_dup_default(ctp, msg, base);
    return e ? e : result < 0 ? -result : 0;
}

static int io_mmap(resmgr_context_t *ctp, io_mmap_t *msg, iofunc_ocb_t *base)
{
    if (!S_ISREG(base->attr->mode))
        return ENODEV;
    unsigned prot = msg->i.prot, required = msg->i.required_prot;
    int saved = base->ioflag;
    /* Only the authenticated memory manager may borrow read access to map an
       execute-only OCB. Keep the actual OCB identity for the helper's mapping. */
    if ((saved & _IO_FLAG_EX) && ctp->info.pid == SYSMGR_PID && ctp->info.nd == ND_LOCAL_NODE)
        base->ioflag |= _IO_FLAG_RD;
    struct open_file *previous = clone_source;
    clone_source = (struct open_file *)base;
    int result = iofunc_mmap_default(ctp, msg, base);
    clone_source = previous;
    base->ioflag = saved;
    if (trace_messages)
        fprintf(stderr,
                "QFuse mmap prot=%x required=%x ioflag=%x mode=%o mount=%x result=%d caller=%d\n",
                prot, required, base->ioflag, base->attr->mode, base->attr->mount->flags, result,
                ctp->info.pid);
    return result;
}

static int io_openfd(resmgr_context_t *ctp, io_openfd_t *msg, iofunc_ocb_t *base)
{
    struct open_file *previous = clone_source;
    clone_source = (struct open_file *)base;
    int result = iofunc_openfd_default(ctp, msg, base);
    clone_source = previous;
    return result;
}
