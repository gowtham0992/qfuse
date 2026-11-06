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
#include "inflight_protocol.h"

extern int qf_upstream_passthrough_main(int, char **);
static int (*upstream_read)(const char *, char *, size_t, off_t, struct fuse_file_info *);
static int notify_fd = -1, release_fd = -1, bypass, force_eof;
static const char *held_path;

static int held_read(const char *path, char *out, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
    if (force_eof)
        return 0;
    if (offset == QF_INFLIGHT_CHUNK && held_path && !strcmp(path, held_path)) {
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
