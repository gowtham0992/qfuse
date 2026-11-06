/* SPDX-License-Identifier: MIT
 * Native behavior oracle: run the same operations on two isolated trees.
 */
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL conformance line %d: %s errno=%d (%s)\n", __LINE__, #x, errno,   \
                    strerror(errno));                                                              \
            exit(1);                                                                               \
        }                                                                                          \
        checks++;                                                                                  \
    } while (0)
static char native[512], view[1024];
static pid_t server;
static void stop(void)
{
    if (server > 0) {
        kill(server, SIGTERM);
        waitpid(server, NULL, 0);
        server = 0;
    }
}
static void timeout(int s)
{
    (void)s;
    if (server > 0)
        kill(server, SIGKILL);
    _exit(124);
}
static void path(char *p, size_t n, const char *root, const char *name)
{
    CHECK(snprintf(p, n, "%s/%s", root, name) > 0);
}
static void exercise(const char *root, int *errors)
{
    char a[1200], b[1200], d[1200], buf[32] = {0};
    struct stat st;
    struct statvfs fs;
    path(a, sizeof(a), root, "file");
    path(b, sizeof(b), root, "renamed");
    path(d, sizeof(d), root, "dir");
    int fd = open(a, O_CREAT | O_EXCL | O_RDWR, 0640);
    CHECK(fd >= 0);
    CHECK(write(fd, "abcdefgh", 8) == 8);
    CHECK(lseek(fd, 0, SEEK_CUR) == 8);
    CHECK(pwrite(fd, "XY", 2, 2) == 2);
    CHECK(lseek(fd, 0, SEEK_CUR) == 8);
    CHECK(pread(fd, buf, 8, 0) == 8 && !memcmp(buf, "abXYefgh", 8));
    CHECK(fstat(fd, &st) == 0 && st.st_size == 8);
    CHECK(ftruncate(fd, 4) == 0);
    CHECK(fstat(fd, &st) == 0 && st.st_size == 4);
    CHECK(ftruncate(fd, 12) == 0);
    memset(buf, 1, sizeof(buf));
    CHECK(pread(fd, buf, 8, 4) == 8);
    CHECK(!memcmp(buf, "\0\0\0\0\0\0\0\0", 8));
    CHECK(fchmod(fd, 0600) == 0);
    CHECK(fstat(fd, &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(fsync(fd) == 0);
    CHECK(close(fd) == 0);
    errno = 0;
    CHECK(open(a, O_CREAT | O_EXCL | O_WRONLY, 0600) == -1);
    errors[0] = errno;
    fd = open(a, O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    CHECK(write(fd, "!", 1) == 1);
    CHECK(close(fd) == 0);
    CHECK(stat(a, &st) == 0 && st.st_size == 13);
    CHECK(truncate(a, 3) == 0);
    struct timespec ts[2] = {{1234567890, 123456000}, {1234567891, 987654000}};
    CHECK(utimensat(AT_FDCWD, a, ts, 0) == 0);
    CHECK(stat(a, &st) == 0 && st.st_mtime == 1234567891);
    CHECK(rename(a, b) == 0);
    errno = 0;
    CHECK(stat(a, &st) == -1);
    errors[1] = errno;
    CHECK(mkdir(d, 0750) == 0);
    errno = 0;
    CHECK(mkdir(d, 0750) == -1);
    errors[2] = errno;
    char child[1200];
    path(child, sizeof(child), d, "child");
    fd = open(child, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    errno = 0;
    CHECK(rmdir(d) == -1);
    errors[3] = errno;
    CHECK(unlink(child) == 0 && rmdir(d) == 0);
    CHECK(symlink("renamed", a) == 0);
    int lr = lstat(a, &st);
    if (lr || !S_ISLNK(st.st_mode))
        fprintf(stderr, "lstat result=%d mode=%o path=%s\n", lr, (unsigned)st.st_mode, a);
    CHECK(lr == 0 && S_ISLNK(st.st_mode));
    memset(buf, 0, sizeof(buf));
    ssize_t link_size = readlink(a, buf, sizeof(buf));
    if (link_size != 7 || memcmp(buf, "renamed", 7))
        fprintf(stderr, "readlink size=%ld content=%s\n", (long)link_size, buf);
    CHECK(link_size == 7 && !memcmp(buf, "renamed", 7));
    fd = open(a, O_RDONLY);
    CHECK(fd >= 0);
    memset(buf, 0, sizeof(buf));
    CHECK(read(fd, buf, 3) == 3 && !memcmp(buf, "abX", 3));
    CHECK(close(fd) == 0);

    /* Open-file lifetime across rename, replacement, unlink and pathname reuse. */
    int held = open(b, O_RDWR);
    CHECK(held >= 0);
    CHECK(rename(b, b) == 0);
    char replacement[1200];
    path(replacement, sizeof(replacement), root, "replacement");
    int other = open(replacement, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(other >= 0 && write(other, "old", 3) == 3);
    CHECK(rename(b, replacement) == 0);
    CHECK(pread(other, buf, 3, 0) == 3 && !memcmp(buf, "old", 3));
    CHECK(fstat(other, &st) == 0 && st.st_nlink == 0 && close(other) == 0);
    CHECK(unlink(replacement) == 0);
    CHECK(fstat(held, &st) == 0 && st.st_nlink == 0);
    CHECK(pread(held, buf, 3, 0) == 3 && !memcmp(buf, "abX", 3));
    CHECK(pwrite(held, "Z", 1, 1) == 1 && ftruncate(held, 2) == 0);
    fd = open(b, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(fd >= 0 && write(fd, "new", 3) == 3 && close(fd) == 0);
    CHECK(pread(held, buf, 2, 0) == 2 && !memcmp(buf, "aZ", 2));
    DIR *active = opendir(root);
    CHECK(active);
    struct dirent *item;
    while ((item = readdir(active)))
        CHECK(strncmp(item->d_name, ".qfuse-hidden.", 14) != 0);
    CHECK(closedir(active) == 0 && close(held) == 0);
    /* A symlink in a parent component must redirect the rest of the path. */
    CHECK(mkdir(d, 0700) == 0);
    path(child, sizeof(child), d, "child");
    fd = open(child, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    CHECK(unlink(a) == 0 && symlink("dir", a) == 0);
    char via_link[1200];
    path(via_link, sizeof(via_link), a, "child");
    CHECK(stat(via_link, &st) == 0 && S_ISREG(st.st_mode));
    CHECK(unlink(via_link) == 0);
    fd = open(via_link, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    CHECK(unlink(child) == 0 && rmdir(d) == 0);
    DIR *listing = opendir(root);
    CHECK(listing);
    struct dirent *entry;
    while ((entry = readdir(listing)))
        CHECK(strncmp(entry->d_name, ".qfuse-hidden.", 14) != 0);
    CHECK(closedir(listing) == 0);
    CHECK(statvfs(root, &fs) == 0 && fs.f_bsize > 0 && fs.f_namemax > 0);
    errno = 0;
    CHECK(unlink(d) == -1);
    errors[4] = errno;
    CHECK(unlink(a) == 0 && unlink(b) == 0);
    errno = 0;
    CHECK(open(b, O_RDONLY) == -1);
    errors[5] = errno;
    CHECK(rmdir(root) == 0);
}
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGALRM, timeout);
    alarm(180);
    atexit(stop);
    strcpy(native, "/data/qfuse-conformance.XXXXXX");
    CHECK(mkdtemp(native));
    char direct[600], backing[600];
    path(direct, sizeof(direct), native, "direct");
    path(backing, sizeof(backing), native, "backing");
    CHECK(mkdir(direct, 0700) == 0 && mkdir(backing, 0700) == 0);
    snprintf(view, sizeof(view), "/qfuse-conformance%s/backing", native);
    server = fork();
    CHECK(server >= 0);
    if (!server) {
        const char *bin = getenv("QFUSE_PASSTHROUGH_BINARY");
        execl(bin ? bin : "/data/qfuse/qfuse-passthrough", "passthrough", "-f",
              "/qfuse-conformance", NULL);
        _exit(127);
    }
    for (int i = 0; i < 200; i++) {
        struct stat st;
        if (!stat(view, &st))
            break;
        struct timespec t = {0, 100000000};
        nanosleep(&t, NULL);
        CHECK(i < 199);
    }
    int expected[6], actual[6];
    exercise(direct, expected);
    printf("PASS native oracle: %d assertions\n", checks);
    exercise(view, actual);
    CHECK(!memcmp(expected, actual, sizeof(expected)));
    stop();
    CHECK(rmdir(native) == 0);
    alarm(0);
    printf("PASS writable differential conformance: %d assertions, 6 exact errno comparisons\n",
           checks);
    return 0;
}
