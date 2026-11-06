/* SPDX-License-Identifier: MIT
 * Test executable links the GPLv2 upstream hello fixture. Not a mount backend.
 */
#include <qfuse/core.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

int qf_upstream_hello_main(int argc, char **argv);
static int upstream_called;
static const struct qf_identity caller = {1001, 1002, 1003, 0022};

struct listing { int count; char names[3][32]; };
static int fill(void *buf, const char *name, const struct stat *st,
                 off_t offset, enum fuse_fill_dir_flags flags)
{
    (void)st; (void)offset; (void)flags;
    struct listing *list = buf;
    assert(list->count < 3);
    assert(strlen(name) < sizeof(list->names[0]));
    strcpy(list->names[list->count++], name);
    return 0;
}

/* Test-only interception of the upstream application's entry to libfuse. */
int fuse_main_real(int argc, char **argv, const struct fuse_operations *ops,
                    size_t size, void *data)
{
    (void)argc; (void)argv;
    struct qf_session s;
    struct qf_request r;
    struct stat st;
    struct fuse_file_info fi = {0};
    char buf[64] = {0};
    upstream_called++;
    assert(qf_session_start(&s, ops, size, data, caller) == 0);
    assert(s.config.kernel_cache == 1); /* Upstream init was invoked. */
    assert(qf_request_init(&r, &s, caller) == 0);
    assert(qf_getattr(&r, "/", &st, NULL) == 0 && S_ISDIR(st.st_mode));
    assert(qf_getattr(&r, "/greeting", &st, NULL) == 0);
    assert(S_ISREG(st.st_mode) && st.st_size == 13);
    assert(qf_getattr(&r, "/missing", &st, NULL) == -ENOENT);
    struct listing list = {0};
    assert(qf_readdir(&r, "/", &list, fill, 0, &fi) == 0);
    assert(list.count == 3);
    assert(strcmp(list.names[0], ".") == 0);
    assert(strcmp(list.names[1], "..") == 0);
    assert(strcmp(list.names[2], "greeting") == 0);
    fi.flags = O_WRONLY;
    assert(qf_open(&r, "/greeting", &fi) == -EACCES);
    fi.flags = O_RDONLY;
    assert(qf_open(&r, "/greeting", &fi) == 0);
    assert(qf_read(&r, "/greeting", buf, sizeof(buf), 0, &fi) == 13);
    assert(memcmp(buf, "QNX callback!", 13) == 0);
    assert(qf_read(&r, "/greeting", buf, 4, 4, &fi) == 4);
    assert(memcmp(buf, "call", 4) == 0);
    assert(qf_read(&r, "/greeting", buf, sizeof(buf), 13, &fi) == 0);
    assert(qf_read(&r, "/greeting", buf, sizeof(buf), -1, &fi) == -EINVAL);
    assert(qf_read(&r, "/greeting", NULL, 0, 0, &fi) == 0);
    assert(qf_read(&r, "/greeting", NULL, 1, 0, &fi) == -EINVAL);
    assert(qf_read(&r, "/greeting", buf, (size_t)INT_MAX + 1, 0, &fi) == -EOVERFLOW);
    const char *bad[] = {"relative", "/../hello", "/a/../hello", "/./hello", "//hello", "/hello/"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++)
        assert(qf_getattr(&r, bad[i], &st, NULL) == -EINVAL);
    char long_path[4098];
    memset(long_path, 'a', sizeof(long_path)); long_path[0] = '/'; long_path[4097] = 0;
    assert(qf_getattr(&r, long_path, &st, NULL) == -ENAMETOOLONG);
    assert(qf_release(&r, "/greeting", &fi) == 0);
    assert(fuse_get_context() == NULL);
    qf_session_stop(&s);
    assert(qf_read(&r, "/greeting", buf, 1, 0, &fi) == -EINVAL);
    puts("PASS upstream hello: options, init, metadata, listing, open, reads, errors, shutdown");
    return 0;
}

static int marker, destroyed;
static void *initialize(struct fuse_conn_info *conn, struct fuse_config *config)
{
    (void)config;
    assert(conn->capable == 0);
    assert(fuse_get_context()->private_data == &destroyed);
    return &marker;
}
static void destroy(void *data)
{
    assert(data == &marker && fuse_get_context()->private_data == &marker);
    destroyed++;
}
static int context_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    (void)path; (void)st; (void)fi;
    assert(fuse_get_context()->private_data == &marker);
    assert(fuse_get_context()->gid == 1002);
    assert(fuse_get_context()->pid == 1003);
    return (int)fuse_get_context()->uid;
}
static int oversize_read(const char *p, char *b, size_t n, off_t o, struct fuse_file_info *fi)
{
    (void)p; (void)b; (void)n; (void)o; (void)fi;
    return 2;
}
static void *worker(void *arg)
{
    struct qf_request *r = arg;
    struct stat st;
    for (int i = 0; i < 10000; i++) {
        assert(qf_getattr(r, "/", &st, NULL) == (int)r->context.uid);
        assert(fuse_get_context() == NULL);
    }
    return NULL;
}
static void test_context_and_lifecycle(void)
{
    struct fuse_operations ops = {.init=initialize, .destroy=destroy,
        .getattr=context_getattr, .read=oversize_read};
    struct qf_session s;
    assert(qf_session_start(&s, &ops, sizeof(ops), &destroyed, caller) == 0);
    struct qf_request first, second;
    struct qf_identity other = caller; other.uid = 2001;
    assert(qf_request_init(&first, &s, caller) == 0);
    assert(qf_request_init(&second, &s, other) == 0);
    pthread_t a, b;
    assert(pthread_create(&a, NULL, worker, &first) == 0);
    assert(pthread_create(&b, NULL, worker, &second) == 0);
    assert(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0);
    struct fuse_file_info fi = {0}; char buf[1];
    assert(qf_read(&first, "/", buf, 1, 0, &fi) == -EIO);
    assert(qf_readdir(&first, "/", NULL, fill, 0, &fi) == -ENOSYS);
    qf_session_stop(&s); qf_session_stop(&s);
    assert(destroyed == 1 && fuse_get_context() == NULL);
    ops = (struct fuse_operations){0};
    assert(qf_session_start(&s, &ops, sizeof(ops), NULL, caller) == 0);
    assert(qf_request_init(&first, &s, caller) == 0);
    struct stat st;
    assert(qf_getattr(&first, "/", &st, NULL) == -ENOSYS);
    assert(qf_read(&first, "/", buf, 1, 0, &fi) == -ENOSYS);
    assert(qf_open(&first, "/", &fi) == 0);
    assert(qf_release(&first, "/", &fi) == 0);
    qf_session_stop(&s);
    puts("PASS core: caller context isolation, lifecycle, missing callbacks, oversized replies");
}

int main(void)
{
    struct utsname platform;
    assert(uname(&platform) == 0);
    printf("Callback test platform: %s %s %s\n", platform.sysname, platform.release, platform.machine);
    char *args[] = {"hello", "--name=greeting", "--contents=QNX callback!", "unused-mountpoint", NULL};
    assert(qf_upstream_hello_main(4, args) == 0);
    assert(upstream_called == 1);
    test_context_and_lifecycle();
    puts("CALLBACK TESTS ONLY: no filesystem mount has been verified in this run.");
    return 0;
}
