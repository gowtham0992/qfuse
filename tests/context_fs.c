/* SPDX-License-Identifier: MIT
 * Test filesystem for native credential and directory-handle checks.
 */
#define FUSE_USE_VERSION 31
#include <fuse.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

static int token;
static int transport_ready = 1;
static int file_handles, directory_handles, callback_error;
static int valid_context(void)
{
    return fuse_get_context() && fuse_get_context()->private_data == &token;
}
static void *initialize(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn; (void)cfg;
    if (!transport_ready || !valid_context()) _exit(44);
    return &token;
}
static int attributes(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    if (!valid_context()) return -EIO;
    if (fi && (!strcmp(path, "/") || !strcmp(path, "/private"))) return -EIO;
    memset(st, 0, sizeof(*st));
    st->st_nlink = 1;
    if (!strcmp(path, "/")) { st->st_mode = S_IFDIR | 0755; st->st_nlink = 2; }
    else if (!strcmp(path, "/private")) { st->st_mode = S_IFDIR | 0700; st->st_nlink = 2; }
    else if (!strcmp(path, "/identity") || !strcmp(path, "/private/secret")) {
        st->st_mode = S_IFREG | 0444;
        char contents[128];
        if (!strcmp(path, "/identity"))
            st->st_size = snprintf(contents, sizeof(contents), "%u %u %ld\n",
                (unsigned)fuse_get_context()->uid, (unsigned)fuse_get_context()->gid,
                (long)fuse_get_context()->pid);
        else st->st_size = strlen("protected\n");
    } else return -ENOENT;
    return 0;
}
static int open_file(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    if (!valid_context() || (fi->flags & O_ACCMODE) != O_RDONLY) return -EACCES;
    fi->fh = 0xc0de;
    file_handles++;
    return 0;
}
static int read_file(const char *path, char *buf, size_t count, off_t offset, struct fuse_file_info *fi)
{
    if (!valid_context() || fi->fh != 0xc0de) { callback_error = 1; return -EIO; }
    char data[128];
    if (!strcmp(path, "/identity"))
        snprintf(data, sizeof(data), "%u %u %ld\n", (unsigned)fuse_get_context()->uid,
            (unsigned)fuse_get_context()->gid, (long)fuse_get_context()->pid);
    else snprintf(data, sizeof(data), "protected\n");
    size_t length = strlen(data);
    if ((size_t)offset >= length) return 0;
    if (count > length - (size_t)offset) count = length - (size_t)offset;
    memcpy(buf, data + offset, count);
    return count;
}
static int release_file(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    if (!valid_context() || fi->fh != 0xc0de) return -EIO;
    fi->fh = 0;
    file_handles--;
    return 0;
}
static int open_dir(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    if (!valid_context()) return -EIO;
    fi->fh = 0xd1;
    directory_handles++;
    return 0;
}
static int read_dir(const char *path, void *buf, fuse_fill_dir_t fill, off_t offset,
                     struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
    (void)offset; (void)flags;
    if (!valid_context() || fi->fh != 0xd1) { callback_error = 1; return -EIO; }
    if (fill(buf, ".", NULL, 0, 0) || fill(buf, "..", NULL, 0, 0)) return 0;
    if (!strcmp(path, "/")) {
        if (fill(buf, "identity", NULL, 0, 0)) return 0;
        fill(buf, "private", NULL, 0, 0);
    } else fill(buf, "secret", NULL, 0, 0);
    return 0;
}
static int release_dir(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    if (!valid_context() || fi->fh != 0xd1) return -EIO;
    fi->fh = 0;
    directory_handles--;
    return 0;
}
static void destroy(void *data)
{
    if (data != &token || file_handles || directory_handles || callback_error) _exit(43);
}
int main(int argc, char **argv)
{
    struct fuse_operations ops = {.init=initialize, .getattr=attributes, .open=open_file, .read=read_file,
        .release=release_file, .opendir=open_dir, .readdir=read_dir, .releasedir=release_dir, .destroy=destroy};
    if (getenv("QFUSE_CONTEXT_LIFECYCLE")) {
        transport_ready = 0;
        char *options[] = {"context", "-o", "ro,subtype=qfuse-test,fsname=context", NULL};
        struct fuse_args args = FUSE_ARGS_INIT(3, options);
        struct fuse *f = fuse_new(&args, &ops, sizeof(ops), &token);
        if (!f || fuse_set_signal_handlers(fuse_get_session(f))) return 45;
        if (fuse_mount(f, "/") != -1 || errno != EINVAL) return 46;
        if (argc < 2 || fuse_mount(f, argv[argc - 1])) return 47;
        if (fuse_mount(f, argv[argc - 1]) != -1 || errno != EBUSY) return 48;
        if (fuse_loop_mt(f, 1) != -ENOTSUP) return 49;
        if (fuse_daemonize(0) != -1 || errno != ENOTSUP) return 50;
        transport_ready = 1;
        if (fuse_daemonize(1)) return 51;
        int result = fuse_loop(f);
        fuse_remove_signal_handlers(fuse_get_session(f));
        fuse_unmount(f);
        fuse_destroy(f);
        return result ? 52 : 0;
    }
    return fuse_main(argc, argv, &ops, &token);
}
