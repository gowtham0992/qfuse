/* SPDX-License-Identifier: MIT */
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
static pid_t server;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL SSHFS write line %d: %s errno=%d (%s)\n", __LINE__, #x, errno,   \
                    strerror(errno));                                                              \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static void stop(void)
{
    if (server > 0) {
        kill(server, SIGTERM);
        int st;
        waitpid(server, &st, 0);
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
static unsigned char sample(size_t i)
{
    return (unsigned char)((i * 37) ^ (i >> 8) ^ (i >> 16));
}
static void mount_remote(void)
{
    server = fork();
    CHECK(server >= 0);
    if (!server) {
        setenv("LD_LIBRARY_PATH", "/data/qfuse/lib", 1);
        execl(getenv("QFUSE_SSHFS_BINARY") ? getenv("QFUSE_SSHFS_BINARY")
                                           : "/data/qfuse/qfuse-sshfs",
              "sshfs", "qfuse-writable:/data", "/data/qfuse-write-mount", "-f", "-F",
              "/data/qfuse/write_ssh_config", "-o", "rw,dir_cache=no,no_readahead,sync_readdir",
              NULL);
        _exit(127);
    }
    for (int i = 0; i < 200; i++) {
        struct stat st;
        if (!stat("/data/qfuse-write-mount/greeting.txt", &st))
            return;
        int status;
        if (waitpid(server, &status, WNOHANG) == server) {
            server = 0;
            CHECK(!"SSHFS exited early");
        }
        struct timespec t = {0, 100000000};
        nanosleep(&t, NULL);
    }
    CHECK(!"SSHFS readiness");
}
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGALRM, timeout);
    alarm(240);
    atexit(stop);
    CHECK(mkdir("/data/qfuse-write-mount", 0700) == 0 || errno == EEXIST);
    mount_remote();
    const char *dir = "/data/qfuse-write-mount/created-dir",
               *file = "/data/qfuse-write-mount/created-dir/binary",
               *renamed = "/data/qfuse-write-mount/created-dir/renamed",
               *link = "/data/qfuse-write-mount/created-dir/link";
    CHECK(mkdir(dir, 0700) == 0);
    int fd = open(file, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0);
    unsigned char data[65536], back[65536];
    size_t total = 0;
    while (total < 200037) {
        size_t n = 200037 - total;
        if (n > sizeof(data))
            n = sizeof(data);
        for (size_t i = 0; i < n; i++)
            data[i] = sample(total + i);
        CHECK(write(fd, data, n) == (ssize_t)n);
        total += n;
    }
    CHECK(fsync(fd) == 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    total = 0;
    for (;;) {
        ssize_t n = read(fd, back, sizeof(back));
        CHECK(n >= 0);
        if (!n)
            break;
        for (ssize_t i = 0; i < n; i++)
            CHECK(back[i] == sample(total + i));
        total += n;
    }
    CHECK(total == 200037);
    CHECK(pwrite(fd, "PATCH", 5, 32766) == 5);
    CHECK(pread(fd, back, 5, 32766) == 5 && !memcmp(back, "PATCH", 5));
    CHECK(ftruncate(fd, 12345) == 0);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == 12345);
    CHECK(fchmod(fd, 0640) == 0);
    CHECK(close(fd) == 0);
    struct timespec times[2] = {{1234567890, 0}, {1234567891, 0}};
    CHECK(utimensat(AT_FDCWD, file, times, 0) == 0);
    CHECK(rename(file, renamed) == 0);
    CHECK(symlink("renamed", link) == 0);
    char target[32] = {0};
    CHECK(readlink(link, target, sizeof(target)) == 7 && !memcmp(target, "renamed", 7));
    fd = open(link, O_RDONLY);
    CHECK(fd >= 0 && read(fd, back, 8) == 8 && close(fd) == 0);
    struct statvfs fs;
    CHECK(statvfs(dir, &fs) == 0 && fs.f_bsize > 0);
    CHECK(unlink(link) == 0);
    stop();
    mount_remote();
    CHECK(stat(renamed, &st) == 0 && st.st_size == 12345 && (st.st_mode & 0777) == 0640 &&
          st.st_mtime == 1234567891);
    fd = open(renamed, O_RDONLY);
    CHECK(fd >= 0 && read(fd, back, 8) == 8 && close(fd) == 0);
    for (size_t i = 0; i < 8; i++)
        CHECK(back[i] == sample(i));
    CHECK(unlink(renamed) == 0 && rmdir(dir) == 0);
    stop();
    CHECK(rmdir("/data/qfuse-write-mount") == 0);
    alarm(0);
    puts("PASS writable SSHFS: 200037 bytes, 64 KiB writes/reads, pwrite, truncate, chmod, "
         "timestamps, rename, symlink, statfs and persisted remount");
    return 0;
}
