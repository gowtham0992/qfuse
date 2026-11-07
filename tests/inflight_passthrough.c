/* SPDX-License-Identifier: MIT
 * Test-only interception of the locked upstream passthrough callback table.
 * The upstream read runs normally, except one selected request can be held
 * before reply so a separate client is demonstrably REPLY-blocked at death.
 */
#define FUSE_USE_VERSION 31
#include <fuse.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>
#include "inflight_protocol.h"

extern int qf_upstream_passthrough_main(int, char **);
static int (*upstream_read)(const char *, char *, size_t, off_t, struct fuse_file_info *);
static int notify_fd = -1, release_fd = -1, bypass, force_eof;
static const char *held_path;
static const char *release_notice_path;
static int (*upstream_release)(const char *, struct fuse_file_info *);
static int notified_release(const char *path, struct fuse_file_info *fi)
{
    int result = upstream_release ? upstream_release(path, fi) : 0;
    if (release_notice_path && !strcmp(path, release_notice_path)) {
        struct fuse_context *context = fuse_get_context();
        struct qf_inflight_event event = {context ? context->pid : -1, -3};
        if (write(notify_fd, &event, sizeof(event)) != sizeof(event))
            return -EIO;
    }
    return result;
}
static int (*upstream_mkdir)(const char *, mode_t);
static int (*upstream_rename)(const char *, const char *, unsigned int);
static int namespace_hold(const char *path, off_t marker)
{
    if (!held_path || strcmp(path, held_path))
        return 0;
    struct fuse_context *context = fuse_get_context();
    if (!context)
        return -EIO;
    struct qf_inflight_event event = {context->pid, marker};
    if (write(notify_fd, &event, sizeof(event)) != sizeof(event))
        return -EIO;
    if (!bypass) {
        char release;
        if (read(release_fd, &release, 1) != 1)
            return -EIO;
    }
    return 0;
}
static int held_mkdir(const char *path, mode_t mode)
{
    int e = namespace_hold(path, -1);
    return e ? e : upstream_mkdir(path, mode);
}
static int held_rename(const char *from, const char *to, unsigned int flags)
{
    int e = namespace_hold(from, -2);
    return e ? e : upstream_rename(from, to, flags);
}

static int (*upstream_flush)(const char *, struct fuse_file_info *);
static int hold_flush, flush_error;
static _Atomic int read_held;
static int checked_flush(const char *path, struct fuse_file_info *fi)
{
    int held = hold_flush && atomic_load(&read_held) && held_path && !strcmp(path, held_path);
    if (held) {
        int e = namespace_hold(path, -4);
        if (e)
            return e;
    }
    int result = flush_error ? -EIO : upstream_flush ? upstream_flush(path, fi) : 0;
    if (held && !result) {
        struct qf_inflight_event event = {fuse_get_context()->pid, -5};
        if (write(notify_fd, &event, sizeof(event)) != sizeof(event))
            return -EIO;
    }
    return result;
}

static int held_read(const char *path, char *out, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
    if (force_eof)
        return 0;
    if (offset == QF_INFLIGHT_CHUNK && held_path && !strcmp(path, held_path)) {
        atomic_store(&read_held, 1);
        struct fuse_context *context = fuse_get_context();
        if (!context)
            return -EIO;
        struct qf_inflight_event event = {context->pid, offset};
        if (write(notify_fd, &event, sizeof(event)) != sizeof(event))
            return -EIO;
        if (!bypass) {
            char release;
            if (read(release_fd, &release, 1) != 1)
                return -EIO;
        }
    }
    return upstream_read(path, out, size, offset, fi);
}

int qf_inflight_main(int argc, char **argv, const struct fuse_operations *ops, size_t size,
                     void *data)
{
    if (!ops || size < offsetof(struct fuse_operations, read) + sizeof(ops->read) ||
        size > sizeof(*ops) || !ops->read)
        return 2;
    struct fuse_operations wrapped = {0};
    memcpy(&wrapped, ops, size);
    const char *notify = getenv("QFUSE_INFLIGHT_NOTIFY_FD");
    const char *release = getenv("QFUSE_INFLIGHT_RELEASE_FD");
    held_path = getenv("QFUSE_INFLIGHT_PATH");
    force_eof = getenv("QFUSE_TEST_FORCE_EOF") != NULL;
    upstream_read = wrapped.read;
    if (notify && release && held_path) {
        notify_fd = atoi(notify);
        release_fd = atoi(release);
        bypass = getenv("QFUSE_INFLIGHT_BYPASS_HOLD") != NULL;
        release_notice_path = getenv("QFUSE_RELEASE_NOTICE_PATH");
        if (release_notice_path) {
            upstream_release = wrapped.release;
            wrapped.release = notified_release;
        }
        hold_flush = getenv("QFUSE_HOLD_FLUSH") != NULL;
        flush_error = getenv("QFUSE_FLUSH_ERROR") != NULL;
        if (hold_flush || flush_error || getenv("QFUSE_ENABLE_FLUSH")) {
            upstream_flush = wrapped.flush;
            wrapped.flush = checked_flush;
        }
        const char *operation = getenv("QFUSE_INFLIGHT_OPERATION");
        if (operation && !strcmp(operation, "mkdir") && wrapped.mkdir) {
            upstream_mkdir = wrapped.mkdir;
            wrapped.mkdir = held_mkdir;
        } else if (operation && !strcmp(operation, "rename") && wrapped.rename) {
            upstream_rename = wrapped.rename;
            wrapped.rename = held_rename;
        } else
            wrapped.read = held_read;
    }
    if (force_eof)
        wrapped.read = held_read;
    return fuse_main_real(argc, argv, &wrapped, size, data);
}

int main(int argc, char **argv)
{
    return qf_upstream_passthrough_main(argc, argv);
}
