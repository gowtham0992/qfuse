/* SPDX-License-Identifier: MIT
 * Independent native client for a real SSHFS/SFTP mount.
 */
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t server_pid;
static const char *mountpoint = "/data/qfuse-sshfs-mount";
static void stop_server(void);
#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "FAIL SSHFS line %d: %s (errno=%d: %s)\n", __LINE__, #test, errno, strerror(errno)); exit(1); \
} } while (0)

static void timed_out(int number)
{
    (void)number;
    if (server_pid > 0) kill(server_pid, SIGKILL);
    _exit(124);
}
static void stop_server(void)
{
    if (server_pid <= 0) return;
    kill(server_pid, SIGTERM);
    for (int i = 0; i < 200; i++) {
        int status;
        if (waitpid(server_pid, &status, WNOHANG) == server_pid) {
            server_pid = 0;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                fprintf(stderr, "SSHFS abnormal exit status=%d\n", status);
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            return;
        }
        struct timespec pause = {0, 100000000}; nanosleep(&pause, NULL);
    }
    kill(server_pid, SIGKILL); waitpid(server_pid, NULL, 0); server_pid = 0;
    CHECK(!"SSHFS required SIGKILL");
}
static void start_server(void)
{
    server_pid = fork(); CHECK(server_pid >= 0);
    if (!server_pid) {
        const char *binary = getenv("QFUSE_SSHFS_BINARY");
        const char *config = getenv("QFUSE_SSHFS_CONFIG");
        setenv("LD_LIBRARY_PATH", "/data/qfuse/lib", 1);
        execl(binary ? binary : "/data/qfuse/qfuse-sshfs", "sshfs", "qfuse-source:/data", mountpoint,
              "-f", "-s", "-F", config ? config : "/data/qfuse/remote_ssh_config",
              "-o", "ro,dir_cache=no,no_readahead,sync_readdir", NULL);
        _exit(127);
    }
}
static void wait_ready(void)
{
    for (int i = 0; i < 200; i++) {
        struct stat st;
        if (!stat("/data/qfuse-sshfs-mount/greeting.txt", &st)) return;
        int status; pid_t observed = waitpid(server_pid, &status, WNOHANG);
        if (observed == server_pid) {
            server_pid = 0;
            fprintf(stderr, "SSHFS exited before readiness: status=%d\n", status);
            CHECK(!"SSHFS failed to establish its mount");
        }
        CHECK(observed == 0);
        struct timespec pause = {0, 100000000}; nanosleep(&pause, NULL);
    }
    CHECK(!"SSHFS did not become ready");
}
static void compare_remote_binary(void)
{
    int source = open("/data/qfuse/reference-binary.dat", O_RDONLY);
    int view = open("/data/qfuse-sshfs-mount/nested/binary.dat", O_RDONLY);
    CHECK(source >= 0 && view >= 0);
    struct stat a, b;
    CHECK(fstat(source, &a) == 0 && fstat(view, &b) == 0 && a.st_size == b.st_size && b.st_size == 200037);
    unsigned char native[8192], mounted[8192];
    off_t total = 0;
    for (;;) {
        ssize_t n = read(source, native, sizeof(native)); CHECK(n >= 0);
        ssize_t m = read(view, mounted, sizeof(mounted)); CHECK(m >= 0);
        CHECK(n == m && !memcmp(native, mounted, n));
        total += n;
        if (!n) break;
    }
    CHECK(total == 200037);
    CHECK(pread(source, native, 31, 32761) == 31 && pread(view, mounted, 31, 32761) == 31);
    CHECK(!memcmp(native, mounted, 31));
    /* Exceed SSHFS's default 32 KiB SFTP read size in one native request. */
    unsigned char *large_native = malloc(65536), *large_mounted = malloc(65536);
    CHECK(large_native && large_mounted);
    CHECK(pread(source, large_native, 65536, 32761) == 65536);
    CHECK(pread(view, large_mounted, 65536, 32761) == 65536);
    CHECK(!memcmp(large_native, large_mounted, 65536));
    free(large_native); free(large_mounted);
    CHECK(lseek(view, 0, SEEK_CUR) == total);
    CHECK(close(source) == 0 && close(view) == 0);
    puts("PASS SSHFS: 200037 remote binary bytes match a separately transferred native reference; 64 KiB pread and EOF");
}
static void check_mount(void)
{
    int fd = open("/data/qfuse-sshfs-mount/greeting.txt", O_RDONLY); CHECK(fd >= 0);
    char message[128] = {0};
    ssize_t length = read(fd, message, sizeof(message) - 1);
    CHECK(length == (ssize_t)strlen("QFuse SSHFS over authenticated SSH\n"));
    CHECK(!strcmp(message, "QFuse SSHFS over authenticated SSH\n"));
    CHECK(close(fd) == 0);
    fd = open("/data/qfuse-sshfs-mount/empty", O_RDONLY); CHECK(fd >= 0);
    CHECK(read(fd, message, sizeof(message)) == 0 && close(fd) == 0);
    DIR *dir = opendir(mountpoint); CHECK(dir);
    int found = 0; errno = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, "greeting.txt")) found |= 1;
        if (!strcmp(entry->d_name, "nested")) found |= 2;
        if (!strcmp(entry->d_name, "empty")) found |= 4;
    }
    CHECK(errno == 0 && found == 7 && closedir(dir) == 0);
    struct stat st;
    errno = 0; CHECK(stat("/data/qfuse-sshfs-mount/missing", &st) == -1 && errno == ENOENT);
    errno = 0; CHECK(open("/data/qfuse-sshfs-mount/greeting.txt", O_WRONLY) == -1 && errno == EROFS);
    errno = 0; CHECK(open("/data/qfuse-sshfs-mount/new", O_RDONLY | O_CREAT, 0644) == -1 && errno == EROFS);
    compare_remote_binary();
    puts("PASS SSHFS: real authenticated remote greeting, directory, empty file, missing path and read-only errors");
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction action = {0}; action.sa_handler = timed_out;
    sigemptyset(&action.sa_mask); CHECK(sigaction(SIGALRM, &action, NULL) == 0);
    alarm(180);
    atexit(stop_server);
    CHECK(mkdir(mountpoint, 0700) == 0 || errno == EEXIST);
    start_server();
    if (argc == 2 && !strcmp(argv[1], "--expect-connect-failure")) {
        int status;
        CHECK(waitpid(server_pid, &status, 0) == server_pid);
        server_pid = 0;
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) != 0);
        struct stat st;
        CHECK(stat("/data/qfuse-sshfs-mount/greeting.txt", &st) == -1 && errno == ENOENT);
        CHECK(rmdir(mountpoint) == 0);
        puts("PASS SSHFS negative connection: authentication failure exits and detaches the staged mount");
        alarm(0); return 0;
    }
    CHECK(argc == 1);
    wait_ready(); check_mount(); stop_server();
    struct stat st;
    CHECK(stat("/data/qfuse-sshfs-mount/greeting.txt", &st) == -1 && errno == ENOENT);
    start_server(); wait_ready(); check_mount(); stop_server();
    CHECK(rmdir(mountpoint) == 0);
    alarm(0);
    puts("PASS SSHFS: graceful shutdown, detached path and authenticated remount");
    return 0;
}
