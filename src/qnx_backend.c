/* SPDX-License-Identifier: MIT
 * QNX high-level FUSE backend. Concurrent callbacks with bounded admission.
 */
#include <qfuse/core.h>
#include <qfuse/control.h>
#include <qfuse/admin.h>
#include <fuse_lowlevel.h> /* Session lifecycle declarations, not low-level operations. */
#define THREAD_POOL_PARAM_T dispatch_context_t
#include <sys/iofunc.h> /* Establish the iofunc handler types before dispatch.h. */
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/netmgr.h>
#include <sys/rsrcdbmgr.h>
#include <sys/ftype.h>
#include <sys/dcmd_blk.h>
#include <sys/statvfs.h>
#include <sys/mman.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef QFUSE_NODE_LIMIT
#define QFUSE_NODE_LIMIT 8192
#endif
#define NODE_LIMIT QFUSE_NODE_LIMIT
#define NODE_BUCKETS 1024
#define DIRECTORY_LIMIT (1024 * 1024)
#define READ_LIMIT (64 * 1024)

struct node {
    iofunc_attr_t attr; /* Must be first: the resource-manager handle. */
    char *path;
    int retired;
    int hidden;
    unsigned opens, physical_ocbs;
    pthread_mutex_t operation_mutex;
    struct node *next;
    struct node *hash_next;
};
struct open_handle {
    struct fuse_file_info fi;
    unsigned refs;
    _Atomic int interrupted;
    int opened, deferred_flush;
};
struct open_file {
    iofunc_ocb_t base; /* Must be first: the resource-manager OCB. */
    struct open_handle *handle;
    struct node *node;
    int registered, queued_free;
    struct open_file *next_free;
    struct qf_request opener;
    char *entries;
    size_t entries_size;
    int listed;
};
static _Thread_local struct open_file *clone_source;
struct fuse_session {
    struct fuse *owner;
    _Atomic sig_atomic_t interrupted;
    int handlers_installed;
    struct sigaction previous[4];
};
struct fuse {
    struct fuse_operations ops;
    size_t ops_size;
    void *data;
    struct fuse_session session;
    char *mountpoint;
    int mounted;
    _Atomic int running;
    pthread_t loop_thread;
    int readonly, requested_ino;
};
static struct {
    struct qf_session session;
    struct node *nodes;
    struct node *buckets[NODE_BUCKETS];
    size_t node_count;
    ino_t inode_sequence;
    unsigned hidden_count, hidden_sequence, physical_ocbs;
    iofunc_mount_t mount;
    iofunc_funcs_t functions;
    resmgr_connect_funcs_t connect;
    resmgr_io_funcs_t io;
    resmgr_attr_t attributes;
    struct fuse *owner;
    dispatch_t *dispatch;
    int path_id;
    int initialized;
    int device_leased;
    dispatch_context_t *context;
    sigset_t signals;
    _Atomic int stopping;
} server;
static int trace_messages;
static struct fuse_session *signal_session;
static pthread_mutex_t release_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t release_ready = PTHREAD_COND_INITIALIZER;
static struct open_file *release_head, *release_tail;
static int release_stop;
static unsigned release_count, release_active;
static uint64_t release_epoch;
static _Atomic unsigned admin_phase;
static void stop_release_worker(pthread_t);
#include "path_gate.inc"
#include "qnx_requests.inc"
#include "qnx_pool.inc"

static int request(resmgr_context_t *ctp, int flags, struct qf_request *r,
                   struct _client_info **info)
{
    int result = iofunc_client_info_ext(ctp, flags, info, 0);
    if (result != EOK)
        return result;
    struct qf_identity who = {(*info)->cred.euid, (*info)->cred.egid, (*info)->pid, 0};
    result = qf_request_init(r, &server.session, who);
    if (result < 0) {
        iofunc_client_info_ext_free(info);
        return -result;
    }
    return EOK;
}

static void update_attr(iofunc_attr_t *a, const struct stat *st)
{
    a->mode = st->st_mode;
    a->uid = st->st_uid;
    a->gid = st->st_gid;
    a->nbytes = st->st_size;
    a->nlink = st->st_nlink;
    if (server.session.config.use_ino && st->st_ino)
        a->inode = st->st_ino;
    a->atime = st->st_atime;
    a->mtime = st->st_mtime;
    a->ctime = st->st_ctime;
    a->atime_ns = st->st_atim.tv_nsec;
    a->mtime_ns = st->st_mtim.tv_nsec;
    a->ctime_ns = st->st_ctim.tv_nsec;
    a->flags &=
        ~(IOFUNC_ATTR_ATIME | IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME | IOFUNC_ATTR_DIRTY_TIME);
}

static void rehash_nodes(void);

static struct node *node_for(const char *path, const struct stat *st)
{
    unsigned hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++)
        hash = (hash ^ *p) * 16777619u;
    unsigned bucket = hash % NODE_BUCKETS;
    for (struct node *n = server.buckets[bucket]; n; n = n->hash_next) {
        if (!n->retired && !n->hidden && strcmp(n->path, path) == 0) {
            if (st && (!server.owner->running || namespace_exclusive))
                update_attr(&n->attr, st);
            return n;
        }
    }
    if (server.node_count == NODE_LIMIT) {
        struct node **link = &server.nodes;
        while (*link) {
            struct node *old = *link;
            if (old->retired && !old->hidden && !old->opens && !old->attr.count &&
                !old->physical_ocbs) {
                *link = old->next;
                pthread_mutex_destroy(&old->operation_mutex);
                free(old->path);
                free(old);
                server.node_count--;
            } else
                link = &old->next;
        }
        rehash_nodes();
        if (server.node_count == NODE_LIMIT) {
            errno = ENFILE;
            return NULL;
        }
    }
    struct node *n = calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->path = strdup(path);
    if (!n->path) {
        free(n);
        return NULL;
    }
    int mutex_error = init_recursive_mutex(&n->operation_mutex);
    if (mutex_error) {
        free(n->path);
        free(n);
        errno = mutex_error;
        return NULL;
    }
    iofunc_attr_init(&n->attr, st ? st->st_mode : 0, NULL, NULL);
    n->attr.mount = &server.mount;
    server.node_count++;
    n->attr.inode =
        server.session.config.use_ino && st && st->st_ino ? st->st_ino : ++server.inode_sequence;
    if (st)
        update_attr(&n->attr, st);
    n->next = server.nodes;
    server.nodes = n;
    n->hash_next = server.buckets[bucket];
    server.buckets[bucket] = n;
    return n;
}

static iofunc_ocb_t *allocate_ocb(resmgr_context_t *ctp, iofunc_attr_t *attr)
{
    (void)ctp;
    if (server.physical_ocbs == OCB_LIMIT) {
        errno = EMFILE;
        return NULL;
    }
    struct open_file *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->node = (struct node *)attr;
    if (clone_source && clone_source->node == f->node) {
        f->handle = clone_source->handle;
        f->handle->refs++;
        f->opener = clone_source->opener;
        f->registered = 1;
        f->node->opens++;
    } else {
        f->handle = calloc(1, sizeof(*f->handle));
        if (!f->handle) {
            free(f);
            return NULL;
        }
        atomic_init(&f->handle->interrupted, 0);
        f->handle->refs = 1;
    }
    f->node->physical_ocbs++;
    server.physical_ocbs++;
    return &f->base;
}
static void free_ocb_actual(iofunc_ocb_t *base)
{
    struct open_file *f = (struct open_file *)base;
    if (f->registered && f->node->opens)
        f->node->opens--;
    if (f->handle && --f->handle->refs == 0) {
        if (f->handle->opened) {
            if (f->handle->deferred_flush && !S_ISDIR(f->node->attr.mode))
                (void)qf_flush(&f->opener, f->node->path, &f->handle->fi);
            if (S_ISDIR(f->node->attr.mode))
                qf_releasedir(&f->opener, f->node->path, &f->handle->fi);
            else
                qf_release(&f->opener, f->node->path, &f->handle->fi);
        }
        free(f->handle);
    }
    struct node *n = f->node;
    if (n->hidden && !n->opens && n->physical_ocbs == 1) {
        int result = qf_unlink(&f->opener, n->path);
        if (!result) {
            n->hidden = 0;
            n->retired = 1;
            server.hidden_count--;
        } else
            fprintf(stderr, "QFuse: deferred backing cleanup failed (%d)\n", -result);
    }
    n->physical_ocbs--;
    server.physical_ocbs--;
    free(f->entries);
    free(f);
}
static void free_ocb(iofunc_ocb_t *base)
{
    struct open_file *f = (struct open_file *)base;
    if (dispatching) {
        if (!f->queued_free) {
            f->queued_free = 1;
            f->next_free = deferred_ocbs;
            deferred_ocbs = f;
        }
    } else {
        state_lock();
        free_ocb_actual(base);
        state_unlock();
    }
}
/* Publish cleanup only after the SDK has finished using the OCB. A dedicated
 * worker may wait in a third-party release without occupying an IPC receiver. */
static void drain_closed_ocbs(void)
{
    if (!deferred_ocbs)
        return;
    pthread_mutex_lock(&release_mutex);
    while (deferred_ocbs) {
        struct open_file *f = deferred_ocbs;
        deferred_ocbs = f->next_free;
        f->next_free = NULL;
        if (release_tail)
            release_tail->next_free = f;
        else
            release_head = f;
        release_tail = f;
        release_count++;
    }
    release_epoch++;
    pthread_cond_signal(&release_ready);
    pthread_mutex_unlock(&release_mutex);
}
static void *release_worker(void *unused)
{
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&release_mutex);
        uint64_t observed = release_epoch;
        unsigned attempts = release_count;
        pthread_mutex_unlock(&release_mutex);
        struct open_file *f = NULL;
        while (attempts--) {
            pthread_mutex_lock(&release_mutex);
            struct open_file *candidate = release_head;
            if (candidate) {
                release_head = candidate->next_free;
                if (!release_head)
                    release_tail = NULL;
                candidate->next_free = NULL;
                release_count--;
                release_active = 1;
            }
            pthread_mutex_unlock(&release_mutex);
            if (!candidate)
                break;
            /* A namespace wait is not a foreign callback. Do not let one
             * blocked path retain the worker needed by unrelated releases. */
            int e = namespace_node(candidate->node, -1, 0);
            if (!e) {
                f = candidate;
                break;
            }
            if (e != EBUSY && e != EAGAIN && e != ENOMEM)
                abort();
            pthread_mutex_lock(&release_mutex);
            if (release_tail)
                release_tail->next_free = candidate;
            else
                release_head = candidate;
            release_tail = candidate;
            release_count++;
            release_active = 0;
            pthread_mutex_unlock(&release_mutex);
        }
        if (f) {
            state_lock();
            free_ocb_actual(&f->base);
            state_unlock();
            namespace_leave();
            pthread_mutex_lock(&release_mutex);
            release_active = 0;
            pthread_mutex_unlock(&release_mutex);
            continue;
        }
        pthread_mutex_lock(&release_mutex);
        if (!release_head && release_stop) {
            pthread_mutex_unlock(&release_mutex);
            return NULL;
        }
        /* Queue arrival and namespace wakeups increment the epoch while
         * holding this mutex, closing the scan-to-sleep lost-wakeup race. */
        if (release_epoch == observed)
            pthread_cond_wait(&release_ready, &release_mutex);
        pthread_mutex_unlock(&release_mutex);
    }
}
static void stop_release_worker(pthread_t worker)
{
    pthread_mutex_lock(&release_mutex);
    release_stop = 1;
    release_epoch++;
    pthread_cond_signal(&release_ready);
    pthread_mutex_unlock(&release_mutex);
    pthread_join(worker, NULL);
}

#include "qnx_mutations.inc"

static int io_open(resmgr_context_t *ctp, io_open_t *msg, iofunc_attr_t *handle, void *extra)
{
    (void)handle;
    (void)extra;
    if (trace_messages)
        fprintf(stderr, "QFuse open subtype=%u flags=%x mode=%o\n", msg->connect.subtype,
                msg->connect.ioflag, msg->connect.mode);
    char path[4097];
    size_t length = msg->connect.path_len;
    if (!length || length > sizeof(path) - 1)
        return ENAMETOOLONG;
    path[0] = '/';
    if (resmgr_msgget(ctp, path + 1, length, offsetof(struct _io_connect, path)) != (ssize_t)length)
        return EINVAL;
    if (path[length] != 0 || strnlen(path + 1, length) != length - 1)
        return EINVAL;
    struct qf_request r;
    struct _client_info *info = NULL;
    struct node *operation_node = NULL;
    int error = request(ctp, msg->connect.ioflag, &r, &info);
    if (error)
        return error;
    struct stat st;
    if (owned_hidden(path)) {
        error = ENOENT;
        goto done;
    }
    error = redirect_parent(ctp, msg, path, &r, info, !!(msg->connect.ioflag & O_CREAT));
    if (error)
        goto done;
    if (server.owner->readonly &&
        ((msg->connect.ioflag & _IO_FLAG_WR) || (msg->connect.ioflag & (O_TRUNC | O_CREAT)))) {
        error = EROFS;
        goto done;
    }
    int result = qf_getattr(&r, path, &st, NULL);
    int created = 0;
    struct fuse_file_info created_fi = {0};
    unsigned access_mode = msg->connect.ioflag & O_ACCMODE;
    created_fi.flags =
        (msg->connect.ioflag & ~O_ACCMODE) |
        ((access_mode & _IO_FLAG_EX) ? O_RDONLY : (access_mode ? access_mode - 1 : O_RDONLY));
    if (result == -ENOENT && (msg->connect.ioflag & O_CREAT)) {
        result = qf_create(&r, path, msg->connect.mode & 07777, &created_fi);
        if (result < 0) {
            error = -result;
            goto done;
        }
        created = 1;
        error = creation_owner(&r, path, &created_fi, info);
        if (error) {
            qf_release(&r, path, &created_fi);
            qf_unlink(&r, path);
            goto done;
        }
        result = qf_getattr(&r, path, &st, &created_fi);
    } else if (!result && (msg->connect.ioflag & O_CREAT) && (msg->connect.ioflag & O_EXCL)) {
        error = EEXIST;
        goto done;
    }
    if (result < 0) {
        error = -result;
        if (created)
            qf_release(&r, path, &created_fi);
        goto done;
    }
    if (S_ISLNK(st.st_mode) && (msg->connect.mode & S_IFMT) != S_IFLNK &&
        !(msg->connect.ioflag & O_NOFOLLOW)) {
        error = redirect_link(ctp, msg, path, &r, NULL);
        goto done;
    }
    if (S_ISLNK(st.st_mode) && (msg->connect.ioflag & O_NOFOLLOW) &&
        msg->connect.subtype == _IO_CONNECT_OPEN) {
        error = ELOOP;
        goto done;
    }
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
        error = ENOTSUP;
        goto done;
    }
    struct node *n = node_for(path, &st);
    if (!n) {
        error = errno;
        if (created)
            qf_release(&r, path, &created_fi);
        goto done;
    }
    unsigned depth = state_drop();
    waiting_begin();
    int operation_error = operation_lock(&n->operation_mutex);
    state_restore(depth);
    waiting_end();
    if (!operation_error)
        operation_node = n;
    if (operation_error || request_cancelled()) {
        error = operation_error ? operation_error : ECANCELED;
        if (created)
            qf_release(&r, path, &created_fi);
        goto done;
    }
    result = qf_getattr(&r, path, &st, created ? &created_fi : NULL);
    if (result < 0) {
        error = -result;
        goto done;
    }
    update_attr(&n->attr, &st);
    io_open_t checked = *msg;
    checked.connect.ioflag &= ~(O_CREAT | O_EXCL | O_TRUNC);
    error = iofunc_open(ctp, &checked, &n->attr, NULL, info);
    if (error) {
        if (created)
            qf_release(&r, path, &created_fi);
        goto done;
    }
    struct open_file *f = (struct open_file *)allocate_ocb(ctp, &n->attr);
    if (!f) {
        error = ENOMEM;
        if (created)
            qf_release(&r, path, &created_fi);
        goto done;
    }
    f->opener = r;
    /* QNX encodes O_RDONLY/O_WRONLY/O_RDWR as _IO_FLAG_RD/_WR in messages. */
    f->handle->fi.flags =
        (msg->connect.ioflag & ~O_ACCMODE) |
        ((access_mode & _IO_FLAG_EX) ? O_RDONLY : (access_mode ? access_mode - 1 : O_RDONLY));
    if (created) {
        f->handle->fi = created_fi;
        result = 0;
    } else if (S_ISLNK(st.st_mode))
        result = 0;
    else
        result = S_ISDIR(st.st_mode) ? qf_opendir(&r, path, &f->handle->fi)
                                     : qf_open(&r, path, &f->handle->fi);
    if (!result && !created && (msg->connect.ioflag & O_TRUNC) && S_ISREG(st.st_mode)) {
        result = qf_truncate(&r, path, 0, &f->handle->fi);
        if (result < 0)
            qf_release(&r, path, &f->handle->fi);
        else
            n->attr.nbytes = 0;
    }
    if (result < 0) {
        error = -result;
        free_ocb(&f->base);
        goto done;
    }
    f->handle->opened = !S_ISLNK(st.st_mode);
    error = iofunc_ocb_attach(ctp, msg, &f->base, &n->attr, NULL);
    if (!error) {
        n->opens++;
        f->registered = 1;
    }
    if (error)
        free_ocb(&f->base);
done:
    if (operation_node)
        pthread_mutex_unlock(&operation_node->operation_mutex);
    iofunc_client_info_ext_free(&info);
    return error;
}

struct directory_builder {
    struct open_file *file;
    const char *path;
    size_t count;
    int error;
};
static int collect_body(void *data, const char *name, const struct stat *st, off_t cookie,
                        enum fuse_fill_dir_flags flags)
{
    (void)cookie;
    struct directory_builder *b = data;
    if (b->error)
        return 1;
    /* We request ordinary readdir and do not negotiate PLUS metadata. */
    if (flags & FUSE_FILL_DIR_PLUS) {
        b->error = ENOTSUP;
        return 1;
    }
    size_t length = strnlen(name, NAME_MAX + 1);
    if (!length || length > NAME_MAX || strchr(name, '/')) {
        b->error = EIO;
        return 1;
    }
    char path[4097];
    int path_length;
    if (!strcmp(name, ".") || !strcmp(name, "..")) {
        path_length = snprintf(path, sizeof(path), "%s", b->path);
        if (!strcmp(name, "..")) {
            char *slash = strrchr(path, '/');
            if (slash == path)
                path[1] = 0;
            else
                *slash = 0;
        }
    } else {
        path_length =
            snprintf(path, sizeof(path), "%s%s%s", b->path, !strcmp(b->path, "/") ? "" : "/", name);
    }
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        b->error = ENAMETOOLONG;
        return 1;
    }
    if (owned_hidden(path))
        return 0;
    ino_t inode;
    if (server.session.config.use_ino && st && st->st_ino)
        inode = st->st_ino;
    else {
        struct node *node = node_for(path, (flags & FUSE_FILL_DIR_PLUS) ? st : NULL);
        if (!node) {
            b->error = errno;
            return 1;
        }
        inode = node->attr.inode;
    }
    size_t size = dirent_calc_reclen(length + 1);
    if (size > DIRECTORY_LIMIT - b->file->entries_size) {
        b->error = EOVERFLOW;
        return 1;
    }
    char *entries = realloc(b->file->entries, b->file->entries_size + size);
    if (!entries) {
        b->error = ENOMEM;
        return 1;
    }
    b->file->entries = entries;
    struct dirent *entry = (struct dirent *)(entries + b->file->entries_size);
    memset(entry, 0, size);
    entry->d_ino = inode;
    entry->d_offset = ++b->count;
    entry->d_reclen = size;
    entry->d_namelen = length;
    memcpy(entry->d_name, name, length + 1);
    b->file->entries_size += size;
    return 0;
}

static int collect(void *data, const char *name, const struct stat *st, off_t cookie,
                   enum fuse_fill_dir_flags flags)
{
    state_lock();
    int result = request_cancelled() ? 1 : collect_body(data, name, st, cookie, flags);
    state_unlock();
    return result;
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *base)
{
    struct open_file *f = (struct open_file *)base;
    struct node *n = (struct node *)base->attr;
    if (!S_ISREG(base->attr->mode) && !S_ISDIR(base->attr->mode))
        return EINVAL;
    /* QNX marks authenticated loader threads with _NTO_CI_LOADER.
       This exception must never grant ordinary clients read access. */
    iofunc_ocb_t checked = *base;
    if (base->ioflag & _IO_FLAG_EX) {
        struct _client_info *loader = NULL;
        int lookup = iofunc_client_info_ext(ctp, base->ioflag, &loader, 0);
        if (lookup)
            return lookup;
        if ((loader->flags & _NTO_CI_LOADER) ||
            (ctp->info.pid == SYSMGR_PID && ctp->info.nd == ND_LOCAL_NODE))
            checked.ioflag |= _IO_FLAG_RD;
        iofunc_client_info_ext_free(&loader);
    }
    int error = iofunc_read_verify(ctp, msg, &checked, NULL);
    if (trace_messages)
        fprintf(stderr,
                "QFuse read type=%u xtype=%x flags=%x count=%llu verify=%d caller=%d nd=%u "
                "checked=%x\n",
                msg->i.type, msg->i.xtype, base->ioflag,
                (unsigned long long)_IO_READ_GET_NBYTES(msg), error, ctp->info.pid, ctp->info.nd,
                checked.ioflag);
    if (error)
        return error;
    unsigned xtype = msg->i.xtype & _IO_XTYPE_MASK;
    off_t offset = base->offset;
    if (xtype == _IO_XTYPE_OFFSET) {
        struct _xtype_offset x;
        if (resmgr_msgget(ctp, &x, sizeof(x), sizeof(msg->i)) != sizeof(x))
            return EINVAL;
        offset = x.offset;
    } else if (xtype != _IO_XTYPE_NONE &&
               !(xtype == _IO_XTYPE_READDIR && S_ISDIR(base->attr->mode)))
        return ENOSYS;
    if (offset < 0)
        return EINVAL;
    size_t count = _IO_READ_GET_NBYTES(msg);
    if (count > READ_LIMIT)
        count = READ_LIMIT;
    struct qf_request r;
    struct _client_info *info = NULL;
    error = request(ctp, base->ioflag, &r, &info);
    if (error)
        return error;
    char *buffer = NULL;
    size_t length = 0;
    if (S_ISDIR(base->attr->mode)) {
        if (!f->listed) {
            struct directory_builder b = {f, n->path, 0, 0};
            int result = qf_readdir(&r, n->path, &b, collect, 0, &f->handle->fi);
            if (result < 0 || b.error || request_cancelled()) {
                error = request_cancelled() ? _RESMGR_NOREPLY : result < 0 ? -result : b.error;
                free(f->entries);
                f->entries = NULL;
                f->entries_size = 0;
                goto done;
            }
            f->listed = 1;
        }
        size_t position = 0;
        off_t index = 0;
        while (position < f->entries_size && index < offset) {
            position += ((struct dirent *)(f->entries + position))->d_reclen;
            index++;
        }
        if (index != offset) {
            error = EINVAL;
            goto done;
        }
        while (position + length < f->entries_size) {
            struct dirent *entry = (struct dirent *)(f->entries + position + length);
            if ((size_t)entry->d_reclen > count - length)
                break;
            length += entry->d_reclen;
            index++;
        }
        if (!length && position < f->entries_size && count) {
            error = EMSGSIZE;
            goto done;
        }
        _IO_SET_READ_NBYTES(ctp, length);
        resmgr_msgreply(ctp, f->entries ? f->entries + position : NULL, length);
        if (xtype != _IO_XTYPE_OFFSET)
            base->offset = index;
    } else {
        buffer = malloc(count ? count : 1);
        if (!buffer) {
            error = ENOMEM;
            goto done;
        }
        int result = qf_read(&r, n->path, buffer, count, offset, &f->handle->fi);
        if (request_cancelled()) {
            error = _RESMGR_NOREPLY;
            goto done;
        }
        if (result < 0) {
            error = -result;
            goto done;
        }
        length = result;
        if (offset > INT64_MAX - (off_t)length) {
            error = EOVERFLOW;
            goto done;
        }
        _IO_SET_READ_NBYTES(ctp, length);
        resmgr_msgreply(ctp, buffer, length);
        if (xtype == _IO_XTYPE_NONE)
            base->offset = offset + length;
    }
    error = _RESMGR_NOREPLY;
done:
    free(buffer);
    iofunc_client_info_ext_free(&info);
    return error;
}

static int io_stat(resmgr_context_t *ctp, io_stat_t *msg, iofunc_ocb_t *base)
{
    struct open_file *f = (struct open_file *)base;
    struct node *n = (struct node *)base->attr;
    struct qf_request r;
    struct _client_info *info = NULL;
    int error = request(ctp, base->ioflag, &r, &info);
    if (error)
        return error;
    struct stat st;
    /* getattr's handle is a file handle; opendir handles belong only to
       readdir/releasedir. SSHFS uses different types for these handles. */
    int result = qf_getattr(&r, n->path, &st, !S_ISREG(base->attr->mode) ? NULL : &f->handle->fi);
    if (result < 0)
        error = -result;
    else {
        if (n->hidden)
            st.st_nlink = 0;
        update_attr(&n->attr, &st);
        error = iofunc_stat_default(ctp, msg, base);
    }
    iofunc_client_info_ext_free(&info);
    return error;
}

static int io_close(resmgr_context_t *ctp, void *reserved, iofunc_ocb_t *base)
{
    struct open_file *f = (struct open_file *)base;
    struct node *n = (struct node *)base->attr;
    struct qf_request r = f->opener;
    struct _client_info *info = NULL;
    /* Synthetic close during detach may have no client. */
    (void)request(ctp, base->ioflag, &r, &info);
    f->opener = r;
    /* SDK close bookkeeping must not wait for a callback on this file.
     * The SDK retains live message bindings; release is deferred until after
     * its dispatcher unwinds, before returning the context to the pool. */
    if (f->registered) {
        if (n->opens)
            n->opens--;
        f->registered = 0;
    }
    if (info)
        iofunc_client_info_ext_free(&info);
    int result = iofunc_close_ocb_default(ctp, reserved, base);
    return result;
}

#define REJECT_IO(name, type, error)                                                               \
    static int name(resmgr_context_t *c, type *m, iofunc_ocb_t *o)                                 \
    {                                                                                              \
        (void)c;                                                                                   \
        (void)m;                                                                                   \
        (void)o;                                                                                   \
        return error;                                                                              \
    }
REJECT_IO(io_lock, io_lock_t, ENOSYS)
REJECT_IO(io_acl, io_acl_t, ENOSYS)

static int namespace_connect(resmgr_context_t *c)
{
    if (c->info.srcmsglen < (ssize_t)offsetof(struct _io_connect, path))
        return EINVAL;
    char path[4097], from[4097];
    int e = connect_path(c, &c->msg->connect, path);
    if (e)
        return e;
    const char *paths[] = {path, from};
    unsigned count = 1;
    if (c->msg->connect.subtype == _IO_CONNECT_RENAME) {
        e = extra_path(c, &c->msg->connect, from, 1);
        if (e)
            return e;
        count = 2;
    }
    return namespace_enter(paths, count, namespace_mutation(c), 1);
}

#include "qnx_guards.inc"
#include "qnx_admin.inc"

static const int handled_signals[] = {SIGTERM, SIGINT, SIGHUP, SIGPIPE};
static void startup_signal(int number)
{
    if (signal_session)
        signal_session->interrupted = number;
}

struct fuse_session *fuse_get_session(struct fuse *f)
{
    return f ? &f->session : NULL;
}

int fuse_set_signal_handlers(struct fuse_session *session)
{
    if (!session || (signal_session && signal_session != session)) {
        errno = EBUSY;
        return -1;
    }
    if (session->handlers_installed)
        return 0;
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    signal_session = session;
    for (size_t i = 0; i < 4; i++) {
        action.sa_handler = handled_signals[i] == SIGPIPE ? SIG_IGN : startup_signal;
        if (sigaction(handled_signals[i], &action, &session->previous[i])) {
            while (i) {
                --i;
                sigaction(handled_signals[i], &session->previous[i], NULL);
            }
            signal_session = NULL;
            return -1;
        }
    }
    session->handlers_installed = 1;
    return 0;
}

void fuse_remove_signal_handlers(struct fuse_session *session)
{
    if (!session || !session->handlers_installed)
        return;
    for (size_t i = 0; i < 4; i++)
        sigaction(handled_signals[i], &session->previous[i], NULL);
    session->handlers_installed = 0;
    if (signal_session == session)
        signal_session = NULL;
}

static int option_error(void *data, const char *argument, int key, struct fuse_args *out)
{
    (void)data;
    (void)key;
    (void)out;
    fprintf(stderr, "QFuse: unsupported FUSE option: %s\n", argument);
    return -1;
}

struct fuse *fuse_new_31(struct fuse_args *args, const struct fuse_operations *ops, size_t size,
                         void *data)
{
    if (!args || !ops || !size || size > sizeof(*ops)) {
        errno = EINVAL;
        return NULL;
    }
    struct parsed_options {
        int mode, permissions, foreground, single, use_ino, allow_other, fresh;
        char *name, *subtype;
    } parsed = {.mode = -1};
    const struct fuse_opt options[] = {
        {"ro", offsetof(struct parsed_options, mode), 1},
        {"rw", offsetof(struct parsed_options, mode), 0},
        {"default_permissions", offsetof(struct parsed_options, permissions), 1},
        {"use_ino", offsetof(struct parsed_options, use_ino), 1},
        {"allow_other", offsetof(struct parsed_options, allow_other), 1},
        {"attr_timeout=0", offsetof(struct parsed_options, fresh), 1},
        {"entry_timeout=0", offsetof(struct parsed_options, fresh), 1},
        {"negative_timeout=0", offsetof(struct parsed_options, fresh), 1},
        {"-f", offsetof(struct parsed_options, foreground), 1},
        {"-s", offsetof(struct parsed_options, single), 1},
        {"fsname=%s", offsetof(struct parsed_options, name), 0},
        {"subtype=%s", offsetof(struct parsed_options, subtype), 0},
        FUSE_OPT_END};
    struct fuse_args copy = FUSE_ARGS_INIT(0, NULL);
    for (int i = 0; i < args->argc; i++) {
        if (fuse_opt_add_arg(&copy, args->argv[i])) {
            fuse_opt_free_args(&copy);
            return NULL;
        }
    }
    int result = fuse_opt_parse(&copy, &parsed, options, option_error);
    fuse_opt_free_args(&copy);
    free(parsed.name);
    free(parsed.subtype);
    if (result < 0) {
        errno = ENOTSUP;
        return NULL;
    }
    struct fuse *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    memcpy(&f->ops, ops, size);
    atomic_init(&f->running, 0);
    atomic_init(&f->session.interrupted, 0);
    f->ops_size = size;
    f->requested_ino = parsed.use_ino;
    f->data = data;
    f->session.owner = f;
    f->readonly =
        parsed.mode < 0 ? (!f->ops.write && !f->ops.create && !f->ops.mkdir) : parsed.mode;
    return f;
}

static int mount_path_valid(const char *path)
{
    if (!path || path[0] != '/' || !path[1] || strlen(path) > 4096)
        return 0;
    for (const char *p = path + 1; *p;) {
        const char *end = strchr(p, '/');
        size_t size = end ? (size_t)(end - p) : strlen(p);
        if (!size || (size == 1 && p[0] == '.') || (size == 2 && !memcmp(p, "..", 2)))
            return 0;
        if (!end)
            return 1;
        p = end + 1;
    }
    return 0;
}

int fuse_mount(struct fuse *f, const char *path)
{
    if (!f || !mount_path_valid(path)) {
        errno = EINVAL;
        return -1;
    }
    if (f->mounted || server.owner) {
        errno = EBUSY;
        return -1;
    }
    if (geteuid() != 0) {
        errno = EPERM;
        return -1;
    }
    if (f->session.interrupted) {
        errno = EINTR;
        return -1;
    }
    f->mountpoint = strdup(path);
    if (!f->mountpoint)
        return -1;
    memset(&server, 0, sizeof(server));
    server.owner = f;
    server.path_id = -1;
    atomic_init(&server.stopping, 0);
    trace_messages = getenv("QFUSE_TRACE") != NULL;
    if (trace_messages)
        fprintf(stderr, "QFuse server PID=%ld\n", (long)getpid());
    if (iofunc_mount_init(&server.mount, sizeof(server.mount)) != EOK)
        goto failure;
    server.initialized = 1;
    server.mount.dev = rsrcdbmgr_devno_attach(_MAJOR_FSYS, -1, 0);
    if (server.mount.dev == (dev_t)-1)
        goto failure;
    server.device_leased = 1;
    server.functions = (iofunc_funcs_t){.nfuncs = _IOFUNC_NFUNCS,
                                        .ocb_calloc = allocate_ocb,
                                        .ocb_free = free_ocb,
                                        .attr_lock = attr_state_lock,
                                        .attr_unlock = attr_state_unlock,
                                        .attr_trylock = attr_state_trylock};
    server.mount.funcs = &server.functions;
    if (f->ops.fsync)
        server.mount.conf |= IOFUNC_PC_SYNC_IO;
    server.mount.flags |= ST_NOSUID;
    if (f->readonly)
        server.mount.flags |= ST_RDONLY;
    /* Publish the prefix before serving. init/getattr are deferred until loop:
     * SSHFS establishes its SFTP connection between mount and loop. */
    struct stat placeholder = {.st_mode = S_IFDIR | 0755, .st_nlink = 2};
    struct node *root = node_for("/", &placeholder);
    if (!root)
        goto failure;
    resmgr_connect_funcs_t *connect = &server.connect;
    resmgr_io_funcs_t *io = &server.io;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, connect, _RESMGR_IO_NFUNCS, io);
    connect->open = guard_io_open;
    connect->unlink = guard_io_unlink;
    connect->rename = guard_io_rename;
    connect->mknod = guard_io_mknod;
    connect->link = guard_io_link;
    connect->readlink = guard_io_readlink;
    connect->mount = guard_io_mount;
    io->read = guard_io_read;
    io->read64 = guard_io_read;
    io->stat = guard_io_stat;
    io->close_ocb = guard_io_close;
    io->close_dup = guard_io_close_dup;
    io->write = guard_io_write;
    io->write64 = guard_io_write;
    io->chmod = guard_io_chmod;
    io->chown = guard_io_chown;
    io->utime = guard_io_utime;
    io->utime64 = guard_io_utime;
    io->space = guard_io_space;
    io->mmap = guard_io_mmap;
    io->sync = guard_io_sync;
    io->devctl = guard_io_devctl;
    io->openfd = guard_io_openfd;
    io->lock_ocb = lock_ocb;
    io->unlock_ocb = unlock_ocb;
    io->unblock = unblock_io;
    connect->unblock = unblock_connect;
    io->lock = io_lock;
    io->acl = io_acl;
    server.attributes.nparts_max = 1;
    server.attributes.msg_max_size = 8192;
    server.dispatch = dispatch_create();
    if (!server.dispatch)
        goto failure;
    server.path_id = resmgr_attach(server.dispatch, &server.attributes, path, _FTYPE_ANY,
                                   _RESMGR_FLAG_DIR | _RESMGR_FLAG_BEFORE | _RESMGR_FLAG_OPAQUE,
                                   connect, io, &root->attr);
    if (server.path_id == -1)
        goto failure;
    server.context = dispatch_context_alloc(server.dispatch);
    if (!server.context)
        goto failure;
    int admin_error = admin_start();
    if (admin_error) {
        errno = -admin_error;
        goto failure;
    }
    f->mounted = 1;
    fprintf(stderr, "QFuse: mounted %s (%s, dispatch pending)\n", path,
            f->readonly ? "read-only" : "read-write");
    return 0;
failure : {
    int error = errno;
    f->mounted = 1;
    fuse_unmount(f);
    errno = error;
    return -1;
}
}

static int run_loop(struct fuse *f, unsigned workers)
{
    if (!f || !f->mounted || server.owner != f || f->running)
        return -EINVAL;
    if (server.session.ready)
        return -EALREADY;
    if (f->session.interrupted)
        return -EINTR;
    sigemptyset(&server.signals);
    sigaddset(&server.signals, SIGTERM);
    sigaddset(&server.signals, SIGINT);
    sigaddset(&server.signals, SIGHUP);
    sigset_t previous;
    int error = pthread_sigmask(SIG_BLOCK, &server.signals, &previous);
    if (error)
        return -error;
    if (f->session.interrupted) {
        pthread_sigmask(SIG_SETMASK, &previous, NULL);
        return -EINTR;
    }
    struct qf_identity who = {geteuid(), getegid(), getpid(), 0};
    int status = qf_session_start_owned(&server.session, &f->ops, f->ops_size, f->data, who, f);
    if (status < 0)
        goto restore;
    if (f->session.interrupted) {
        status = 0;
        goto restore;
    }
    if (f->requested_ino) server.session.config.use_ino = 1;
    struct qf_request r;
    qf_request_init(&r, &server.session, who);
    struct stat root;
    status = qf_getattr(&r, "/", &root, NULL);
    if (status < 0)
        goto restore;
    if (!S_ISDIR(root.st_mode)) {
        status = -ENOTDIR;
        goto restore;
    }
    if (!node_for("/", &root)) {
        status = -errno;
        goto restore;
    }
    server.session.callback_pause = callback_pause;
    server.session.callback_resume = callback_resume;
    f->loop_thread = pthread_self();
    f->running = 1;
    fprintf(
        stderr,
        "QFuse: serving with %u minimum receiver(s); concurrent callbacks, per-file operation locking\n",
        workers);
    status = serve_pool(workers);
    f->running = 0;
restore:
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return status;
}

void fuse_unmount(struct fuse *f)
{
    if (!f || !f->mounted || server.owner != f)
        return;
    if (f->running) {
        fputs("QFuse: unmount requires the loop to stop first\n", stderr);
        return;
    }
    if (server.path_id != -1)
        resmgr_detach(server.dispatch, server.path_id, _RESMGR_DETACH_ALL | _RESMGR_DETACH_CLOSE);
    if (server.context)
        dispatch_context_free(server.context);
    if (server.dispatch)
        dispatch_destroy(server.dispatch);
    qf_session_stop(&server.session);
    admin_stop();
    for (struct node *n = server.nodes; n;) {
        struct node *next = n->next;
        pthread_mutex_destroy(&n->operation_mutex);
        free(n->path);
        free(n);
        n = next;
    }
    if (server.device_leased)
        rsrcdbmgr_devno_detach(server.mount.dev, 0);
    if (server.initialized)
        iofunc_mount_fini(&server.mount);
    free(f->mountpoint);
    f->mountpoint = NULL;
    f->mounted = 0;
    memset(&server, 0, sizeof(server));
}

void fuse_destroy(struct fuse *f)
{
    if (!f || f->running)
        return;
    fuse_unmount(f);
    fuse_remove_signal_handlers(&f->session);
    free(f);
}

int fuse_version(void)
{
    return FUSE_VERSION;
}
void fuse_exit(struct fuse *f)
{
    if (!f)
        return;
    f->session.interrupted = SIGTERM;
    if (f->running)
        pthread_kill(f->loop_thread, SIGTERM);
}
void fuse_session_exit(struct fuse_session *session)
{
    if (session)
        fuse_exit(session->owner);
}
int fuse_session_exited(struct fuse_session *session)
{
    return session ? !!session->interrupted : 1;
}

int fuse_loop(struct fuse *f)
{
    return run_loop(f, 1);
}
int fuse_loop_mt_31(struct fuse *f, int clone_fd)
{
    if (clone_fd) {
        errno = ENOTSUP;
        return -ENOTSUP;
    }
    return run_loop(f, 4);
}
int fuse_daemonize(int foreground)
{
    if (foreground)
        return 0;
    errno = ENOTSUP;
    return -1;
}
const char *fuse_pkgversion(void)
{
    return "QFuse prototype; libfuse 3.16.2 high-level baseline";
}
void fuse_lowlevel_version(void)
{
    puts("QFuse: low-level FUSE operations are unsupported");
}
void fuse_lib_help(struct fuse_args *args)
{
    (void)args;
    puts("QFuse: high-level API 31; foreground (-f), one worker (-s) or four workers by default; "
         "-o ro/rw. Low-level API and daemonization unsupported.");
}

int fuse_main_real(int argc, char **argv, const struct fuse_operations *ops, size_t size,
                   void *data)
{
    const char *mountpoint = NULL;
    int single = 0;
    struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
    if (fuse_opt_add_arg(&args, argc ? argv[0] : "qfuse"))
        return 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fuse_lib_help(&args);
            fuse_opt_free_args(&args);
            return 0;
        }
        if (!strcmp(argv[i], "-f"))
            continue;
        if (!strcmp(argv[i], "-s")) {
            single = 1;
            continue;
        }
        if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            if (fuse_opt_add_arg(&args, argv[i]) || fuse_opt_add_arg(&args, argv[++i]))
                goto arguments_error;
        } else if (!strncmp(argv[i], "-o", 2)) {
            if (fuse_opt_add_arg(&args, argv[i]))
                goto arguments_error;
        } else if (argv[i][0] == '-' || mountpoint)
            goto arguments_error;
        else
            mountpoint = argv[i];
    }
    struct fuse *f = fuse_new(&args, ops, size, data);
    fuse_opt_free_args(&args);
    if (!f)
        return 1;
    int result = 1;
    if (fuse_set_signal_handlers(fuse_get_session(f)))
        goto done;
    if (fuse_mount(f, mountpoint)) {
        perror("QFuse mount");
        goto done;
    }
    result = (single ? fuse_loop(f) : fuse_loop_mt_31(f, 0)) ? 1 : 0;
done:
    fuse_destroy(f);
    return result;
arguments_error:
    fputs("QFuse: invalid mount arguments\n", stderr);
    fuse_opt_free_args(&args);
    return 2;
}
