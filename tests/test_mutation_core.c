/* SPDX-License-Identifier: MIT */
#include <qfuse/core.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
static char data[32];
static size_t length;
static int calls, over;
static int create(const char *p, mode_t mode, struct fuse_file_info *fi)
{
    assert(!strcmp(p, "/new") && mode == 0600);
    assert(fuse_get_context()->uid == 123);
    calls++;
    fi->fh = 42;
    return 0;
}
static int write_data(const char *p, const char *s, size_t n, off_t off, struct fuse_file_info *fi)
{
    assert(!strcmp(p, "/new") && fi->fh == 42 && fuse_get_context()->uid == 123);
    calls++;
    assert((size_t)off + n <= sizeof(data));
    memcpy(data + off, s, n);
    length = off + n;
    return over ? (int)n + 1 : (int)n;
}
static int truncate_data(const char *p, off_t size, struct fuse_file_info *fi)
{
    assert(!strcmp(p, "/new") && fi->fh == 42);
    calls++;
    length = size;
    return 0;
}
int main(void)
{
    struct fuse_operations ops = {.create = create, .write = write_data, .truncate = truncate_data};
    struct qf_session s;
    struct qf_request r;
    struct qf_identity who = {123, 456, 789, 0};
    assert(!qf_session_start(&s, &ops, sizeof(ops), NULL, who));
    assert(!qf_request_init(&r, &s, who));
    struct fuse_file_info fi = {0};
    assert(!qf_create(&r, "/new", 0600, &fi) && fi.fh == 42);
    assert(qf_write(&r, "/new", "abc", 3, 0, &fi) == 3 && length == 3 && !memcmp(data, "abc", 3));
    assert(qf_write(&r, "/new", "XY", 2, 1, &fi) == 2 && !memcmp(data, "aXY", 3));
    assert(!qf_truncate(&r, "/new", 1, &fi) && length == 1);
    int before = calls;
    assert(qf_write(&r, "/new", "x", 1, -1, &fi) == -EINVAL && calls == before);
    assert(qf_write(&r, "/../new", "x", 1, 0, &fi) == -EINVAL && calls == before);
    assert(qf_create(&r, "/new", 0600, NULL) == -EINVAL && calls == before);
    assert(qf_truncate(&r, "/new", -1, &fi) == -EINVAL && calls == before);
    assert(qf_write(&r, "/new", "x", 1, INT64_MAX, &fi) == -EOVERFLOW && calls == before);
    assert(qf_mkdir(&r, "/dir", 0700) == -ENOSYS);
    over = 1;
    assert(qf_write(&r, "/new", "x", 1, 0, &fi) == -EIO);
    assert(fuse_get_context() == NULL);
    qf_session_stop(&s);
    return 0;
}
