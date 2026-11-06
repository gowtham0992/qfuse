/* SPDX-License-Identifier: MIT
 * Test filesystem: report the actual callback scheduler priority and caller.
 */
#define FUSE_USE_VERSION 31
#include <fuse.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
struct priority_event {
    int priority;
    pid_t caller;
};
static int notify_fd, resume_fd, token;
static void *init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    (void)cfg;
    struct fuse_context *c = fuse_get_context();
    if (!c || !c->fuse || c->private_data != &token)
        _exit(42);
    return &token;
}
static int attributes(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    (void)fi;
    memset(st, 0, sizeof(*st));
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_nlink = 1;
    if (!strcmp(path, "/")) {
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
    } else if (!strcmp(path, "/priority")) {
        st->st_mode = S_IFREG | 0444;
        st->st_size = 64;
    } else
        return -ENOENT;
    return 0;
}
static int data(const char *path, char *out, size_t size, off_t off, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    if (off || !size)
        return 0;
    int policy;
    struct sched_param current;
    if (pthread_getschedparam(pthread_self(), &policy, &current))
        return -EIO;
    struct fuse_context *c = fuse_get_context();
    if (!c || !c->fuse || c->private_data != &token)
        return -EIO;
    struct priority_event event = {current.sched_curpriority, c->pid};
    if (write(notify_fd, &event, sizeof(event)) != sizeof(event))
        return -EIO;
    char go;
    if (read(resume_fd, &go, 1) != 1)
        return -EIO;
    char value[64];
    int len = snprintf(value, sizeof(value), "%d %ld\n", event.priority, (long)event.caller);
    if ((size_t)len > size)
        len = size;
    memcpy(out, value, len);
    return len;
}
int main(int argc, char **argv)
{
    const char *a = getenv("QFUSE_PRIORITY_NOTIFY_FD"), *b = getenv("QFUSE_PRIORITY_RESUME_FD");
    if (!a || !b)
        return 2;
    notify_fd = atoi(a);
    resume_fd = atoi(b);
    struct fuse_operations ops = {.init = init, .getattr = attributes, .read = data};
    return fuse_main(argc, argv, &ops, &token);
}
