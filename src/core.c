/* SPDX-License-Identifier: MIT */
#include <qfuse/core.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

static _Thread_local struct fuse_context *current_context;

struct fuse_context *fuse_get_context(void)
{
    return current_context;
}

static struct fuse_context context_for(struct qf_identity who, void *data)
{
    struct fuse_context context = {0};
    context.uid = who.uid;
    context.gid = who.gid;
    context.pid = who.pid;
    context.umask = who.umask;
    context.private_data = data;
    return context;
}

int qf_session_start_owned(struct qf_session *s, const struct fuse_operations *ops, size_t size,
                           void *data, struct qf_identity who, struct fuse *owner)
{
    if (!s || !ops || size == 0 || size > sizeof(*ops))
        return -EINVAL;
    /* Caller supplies a fresh or stopped session. */
    memset(s, 0, sizeof(*s));
    memcpy(&s->ops, ops, size);
    s->private_data = data;
    s->lifecycle_context = context_for(who, data);
    s->lifecycle_context.fuse = owner;
    if (s->ops.init) {
        struct fuse_conn_info conn = {0}; /* No kernel capabilities advertised. */
        struct fuse_context *saved = current_context;
        current_context = &s->lifecycle_context;
        s->private_data = s->ops.init(&conn, &s->config);
        current_context = saved;
    }
    s->lifecycle_context.private_data = s->private_data;
    s->ready = 1;
    return 0;
}

int qf_session_start(struct qf_session *s, const struct fuse_operations *ops, size_t size,
                     void *data, struct qf_identity who)
{
    return qf_session_start_owned(s, ops, size, data, who, NULL);
}

void qf_session_stop(struct qf_session *s)
{
    if (!s || !s->ready)
        return;
    s->ready = 0;
    if (s->ops.destroy) {
        struct fuse_context *saved = current_context;
        current_context = &s->lifecycle_context;
        s->ops.destroy(s->private_data);
        current_context = saved;
    }
}

int qf_request_init(struct qf_request *r, struct qf_session *s, struct qf_identity who)
{
    if (!r || !s || !s->ready)
        return -EINVAL;
    r->session = s;
    r->context = context_for(who, s->private_data);
    r->context.fuse = s->lifecycle_context.fuse;
    return 0;
}

static int validate(struct qf_request *r, const char *path)
{
    if (!r || !r->session || !r->session->ready)
        return -EINVAL;
    if (!path || path[0] != '/')
        return -EINVAL;
    size_t length = strnlen(path, 4097);
    if (length > 4096)
        return -ENAMETOOLONG;
    /* Adapter must supply canonical, mount-relative absolute paths. */
    for (const char *p = path + 1; *p;) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == 0 || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
            return -EINVAL;
        if (!end)
            break;
        p = end + 1;
        if (!*p)
            return -EINVAL;
    }
    return 0;
}

/* Preserve context across nested callbacks as well as worker threads. */
#define INVOKE(r, expression, output)                                                              \
    do {                                                                                           \
        struct fuse_context *saved = current_context;                                              \
        current_context = &(r)->context;                                                           \
        unsigned token = 0;                                                                        \
        int enter = (r)->session->callback_pause ? (r)->session->callback_pause(&token) : 0;       \
        (output) = enter ? enter : (expression);                                                   \
        if (!enter && (r)->session->callback_resume)                                               \
            (r)->session->callback_resume(token);                                                  \
        current_context = saved;                                                                   \
    } while (0)
#define CALL(r, expression)                                                                        \
    do {                                                                                           \
        int result;                                                                                \
        INVOKE(r, expression, result);                                                             \
        return result;                                                                             \
    } while (0)

int qf_getattr(struct qf_request *r, const char *path, struct stat *st, struct fuse_file_info *fi)
{
    int error = validate(r, path);
    if (error)
        return error;
    if (!st)
        return -EINVAL;
    if (!r->session->ops.getattr)
        return -ENOSYS;
    memset(st, 0, sizeof(*st));
    CALL(r, r->session->ops.getattr(path, st, fi));
}

int qf_open(struct qf_request *r, const char *path, struct fuse_file_info *fi)
{
    int error = validate(r, path);
    if (error)
        return error;
    if (!fi)
        return -EINVAL;
    /* FUSE permits omitting open when no per-handle state is needed. */
    if (!r->session->ops.open)
        return 0;
    CALL(r, r->session->ops.open(path, fi));
}

int qf_read(struct qf_request *r, const char *path, char *buf, size_t size, off_t offset,
            struct fuse_file_info *fi)
{
    int error = validate(r, path);
    if (error)
        return error;
    if ((!buf && size) || !fi || offset < 0)
        return -EINVAL;
    if (size > INT_MAX || (size && offset > INT64_MAX - (off_t)size))
        return -EOVERFLOW;
    if (!r->session->ops.read)
        return -ENOSYS;
    if (size == 0)
        return 0;
    int result;
    INVOKE(r, r->session->ops.read(path, buf, size, offset, fi), result);
    /* A callback that over-reports must not cause an oversized reply. */
    return result > 0 && (size_t)result > size ? -EIO : result;
}

int qf_readdir(struct qf_request *r, const char *path, void *buf, fuse_fill_dir_t filler,
               off_t offset, struct fuse_file_info *fi)
{
    int error = validate(r, path);
    if (error)
        return error;
    if (!filler || !fi || offset < 0)
        return -EINVAL;
    if (!r->session->ops.readdir)
        return -ENOSYS;
    CALL(r, r->session->ops.readdir(path, buf, filler, offset, fi, 0));
}

int qf_release(struct qf_request *r, const char *path, struct fuse_file_info *fi)
{
    int error = validate(r, path);
    if (error)
        return error;
    if (!fi)
        return -EINVAL;
    if (!r->session->ops.release)
        return 0;
    CALL(r, r->session->ops.release(path, fi));
}

int qf_opendir(struct qf_request *r, const char *path, struct fuse_file_info *fi)
{
    int error = validate(r, path);
    if (error)
        return error;
    if (!fi)
        return -EINVAL;
    if (!r->session->ops.opendir)
        return 0;
    CALL(r, r->session->ops.opendir(path, fi));
}

int qf_releasedir(struct qf_request *r, const char *path, struct fuse_file_info *fi)
{
    int error = validate(r, path);
    if (error)
        return error;
    if (!fi)
        return -EINVAL;
    if (!r->session->ops.releasedir)
        return 0;
    CALL(r, r->session->ops.releasedir(path, fi));
}

int qf_create(struct qf_request *r, const char *p, mode_t mode, struct fuse_file_info *fi)
{
    int e = validate(r, p);
    if (e)
        return e;
    if (!fi)
        return -EINVAL;
    if (!r->session->ops.create)
        return -ENOSYS;
    CALL(r, r->session->ops.create(p, mode, fi));
}
int qf_write(struct qf_request *r, const char *p, const char *buf, size_t size, off_t off,
             struct fuse_file_info *fi)
{
    int e = validate(r, p);
    if (e)
        return e;
    if ((!buf && size) || !fi || off < 0)
        return -EINVAL;
    if (size > INT_MAX || (size && off > INT64_MAX - (off_t)size))
        return -EOVERFLOW;
    if (!r->session->ops.write)
        return -ENOSYS;
    if (!size)
        return 0;
    int n;
    INVOKE(r, r->session->ops.write(p, buf, size, off, fi), n);
    return n > 0 && (size_t)n > size ? -EIO : n;
}
#define VALIDATE_OP(name)                                                                          \
    int e = validate(r, p);                                                                        \
    if (e)                                                                                         \
        return e;                                                                                  \
    if (!r->session->ops.name)                                                                     \
    return -ENOSYS
int qf_truncate(struct qf_request *r, const char *p, off_t size, struct fuse_file_info *fi)
{
    VALIDATE_OP(truncate);
    if (size < 0)
        return -EINVAL;
    CALL(r, r->session->ops.truncate(p, size, fi));
}
int qf_mkdir(struct qf_request *r, const char *p, mode_t mode)
{
    VALIDATE_OP(mkdir);
    CALL(r, r->session->ops.mkdir(p, mode));
}
int qf_unlink(struct qf_request *r, const char *p)
{
    VALIDATE_OP(unlink);
    CALL(r, r->session->ops.unlink(p));
}
int qf_rmdir(struct qf_request *r, const char *p)
{
    VALIDATE_OP(rmdir);
    CALL(r, r->session->ops.rmdir(p));
}
int qf_rename(struct qf_request *r, const char *p, const char *to)
{
    VALIDATE_OP(rename);
    e = validate(r, to);
    if (e)
        return e;
    CALL(r, r->session->ops.rename(p, to, 0));
}
int qf_symlink(struct qf_request *r, const char *target, const char *p)
{
    VALIDATE_OP(symlink);
    if (!target)
        return -EINVAL;
    if (strnlen(target, 4097) > 4096)
        return -ENAMETOOLONG;
    CALL(r, r->session->ops.symlink(target, p));
}
int qf_readlink(struct qf_request *r, const char *p, char *buf, size_t size)
{
    VALIDATE_OP(readlink);
    if (!buf || !size)
        return -EINVAL;
    memset(buf, 0, size);
    int result;
    INVOKE(r, r->session->ops.readlink(p, buf, size), result);
    return result >= 0 && !memchr(buf, 0, size) ? -EIO : result;
}
int qf_chmod(struct qf_request *r, const char *p, mode_t mode, struct fuse_file_info *fi)
{
    VALIDATE_OP(chmod);
    CALL(r, r->session->ops.chmod(p, mode, fi));
}
int qf_chown(struct qf_request *r, const char *p, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
    VALIDATE_OP(chown);
    CALL(r, r->session->ops.chown(p, uid, gid, fi));
}
int qf_utimens(struct qf_request *r, const char *p, const struct timespec ts[2],
               struct fuse_file_info *fi)
{
    VALIDATE_OP(utimens);
    if (!ts)
        return -EINVAL;
    CALL(r, r->session->ops.utimens(p, ts, fi));
}
int qf_statfs(struct qf_request *r, const char *p, struct statvfs *st)
{
    VALIDATE_OP(statfs);
    if (!st)
        return -EINVAL;
    memset(st, 0, sizeof(*st));
    CALL(r, r->session->ops.statfs(p, st));
}
int qf_fsync(struct qf_request *r, const char *p, int data, struct fuse_file_info *fi)
{
    VALIDATE_OP(fsync);
    if (!fi)
        return -EINVAL;
    CALL(r, r->session->ops.fsync(p, data, fi));
}
int qf_flush(struct qf_request *r, const char *p, struct fuse_file_info *fi)
{
    int e = validate(r, p);
    if (e)
        return e;
    if (!fi)
        return -EINVAL;
    if (!r->session->ops.flush)
        return 0;
    CALL(r, r->session->ops.flush(p, fi));
}
