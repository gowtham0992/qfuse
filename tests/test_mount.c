/* SPDX-License-Identifier: MIT
 * Black-box POSIX and QNX protocol tests. No FUSE or QFuse headers are linked here.
 */
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/neutrino.h>
#include <sys/iomsg.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t server_pid;
static void timeout_test(int signal_number)
{
    (void)signal_number;
    if (server_pid > 0) kill(server_pid, SIGKILL);
    _exit(124);
}
static void stop_server(void)
{
    if (server_pid <= 0) return;
    kill(server_pid, SIGTERM);
    for (int i = 0; i < 100; i++) {
        int status;
        if (waitpid(server_pid, &status, WNOHANG) == server_pid) {
            server_pid = 0;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                fputs("FAIL: server did not shut down cleanly\n", stderr); exit(1);
            }
            return;
        }
        struct timespec pause = {0, 100000000}; nanosleep(&pause, NULL);
    }
    kill(server_pid, SIGKILL); waitpid(server_pid, NULL, 0); server_pid = 0;
    fputs("FAIL: server required SIGKILL\n", stderr);
    exit(1);
}
#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "FAIL line %d: %s (errno=%d: %s)\n", __LINE__, #test, errno, strerror(errno)); \
    exit(1); } } while (0)

static void start_server(int context_fixture)
{
    server_pid = fork(); CHECK(server_pid >= 0);
    if (server_pid == 0) {
        const char *binary = getenv(context_fixture ? "QFUSE_CONTEXT_BINARY" : "QFUSE_SERVER_BINARY");
        if (context_fixture)
            execl(binary ? binary : "/usr/bin/qfuse-context", "qfuse-context", "-f", "/qfuse-context", NULL);
        else execl(binary ? binary : "/usr/bin/qfuse-hello", "qfuse-hello", "-f", "--name=greeting", "--contents=QNX callback!", "/qfuse-hello", NULL);
        _exit(127);
    }
    for (int i = 0; i < 100; i++) {
        struct stat st;
        if (!stat(context_fixture ? "/qfuse-context/identity" : "/qfuse-hello/greeting", &st)) return;
        int status;
        pid_t observed = waitpid(server_pid, &status, WNOHANG);
        if (observed == server_pid) {
            server_pid = 0;
            fprintf(stderr, "Server exited before readiness: status=%d\n", status);
            CHECK(!"server exited before readiness");
        }
        CHECK(observed == 0);
        struct timespec pause = {0, 100000000}; nanosleep(&pause, NULL);
    }
    CHECK(!"mount did not become ready");
}

static void check_identity(void)
{
    int fd = open("/qfuse-context/identity", O_RDONLY); CHECK(fd >= 0);
    char buf[128] = {0};
    CHECK(read(fd, buf, sizeof(buf) - 1) > 0); CHECK(close(fd) == 0);
    unsigned uid, gid; long pid;
    CHECK(sscanf(buf, "%u %u %ld", &uid, &gid, &pid) == 3);
    CHECK(uid == (unsigned)geteuid() && gid == (unsigned)getegid() && pid == (long)getpid());
}

static void check_context_and_permissions(void)
{
    start_server(1);
    check_identity();
    DIR *dir = opendir("/qfuse-context"); CHECK(dir);
    int count = 0; while (readdir(dir)) count++;
    CHECK(count == 4); CHECK(closedir(dir) == 0);
    int fd = open("/qfuse-context/private/secret", O_RDONLY); CHECK(fd >= 0); CHECK(close(fd) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        server_pid = 0; /* Child must never clean up the parent's server. */
        CHECK(setgid(1000) == 0 && setuid(1000) == 0);
        check_identity();
        struct stat st;
        CHECK(stat("/qfuse-context/private", &st) == 0);
        errno = 0;
        CHECK(open("/qfuse-context/private/secret", O_RDONLY) == -1 && errno == EACCES);
        errno = 0;
        CHECK(stat("/qfuse-context/private/secret", &st) == -1 && errno == EACCES);
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    stop_server();
    puts("PASS native security: per-request UID/GID/PID, private data, directory handles, parent search permissions");
}

static void check_mount(void)
{
    struct stat st;
    CHECK(stat("/qfuse-hello", &st) == 0 && S_ISDIR(st.st_mode));
    ino_t root_inode = st.st_ino;
    CHECK(stat("/qfuse-hello/greeting", &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 13);
    errno = 0; CHECK(stat("/qfuse-hello/missing", &st) == -1 && errno == ENOENT);
    DIR *dir = opendir("/qfuse-hello"); CHECK(dir);
    int found = 0, count = 0;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir))) {
        count++;
        if (!strcmp(entry->d_name, ".")) CHECK(entry->d_ino == root_inode);
        if (!strcmp(entry->d_name, "greeting")) {
            found++;
            CHECK(stat("/qfuse-hello/greeting", &st) == 0 && entry->d_ino == st.st_ino);
        }
    }
    CHECK(errno == 0 && found == 1 && count == 3);
    rewinddir(dir); count = 0;
    while (readdir(dir)) count++;
    CHECK(count == 3); CHECK(closedir(dir) == 0);
    /* O_DIRECTORY/dirfd refer to the path manager here, not the FUSE server.
     * A plain read connection targets the actual resource-manager endpoint. */
    int directory = open("/qfuse-hello", O_RDONLY); CHECK(directory >= 0);
    struct _server_info endpoint;
    CHECK(ConnectServerInfo(0, directory, &endpoint) >= 0);
    CHECK(endpoint.pid == server_pid);
    _Alignas(struct dirent) char small[32];
    io_read_t directory_request = {0};
    directory_request.i.type = _IO_READ;
    directory_request.i.combine_len = sizeof(directory_request.i);
    directory_request.i.xtype = _IO_XTYPE_READDIR | _IO_XFLAG_DIR_STAT_FORM_T64_2008;
    directory_request.i.nbytes = 8;
    errno = 0;
    CHECK(MsgSend(directory, &directory_request, sizeof(directory_request.i), small, 8) == -1 && errno == EMSGSIZE);
    directory_request.i.nbytes = sizeof(small);
    count = 0;
    ssize_t bytes;
    while ((bytes = MsgSend(directory, &directory_request, sizeof(directory_request.i), small, sizeof(small))) > 0) {
        CHECK(bytes >= (ssize_t)offsetof(struct dirent, d_name));
        CHECK(((struct dirent *)small)->d_reclen == bytes);
        count++;
    }
    CHECK(bytes == 0 && count == 3);
    CHECK(close(directory) == 0);
    int fd = open("/qfuse-hello/greeting", O_RDONLY); CHECK(fd >= 0);
    char buf[32] = {0};
    CHECK(read(fd, buf, 4) == 4 && !memcmp(buf, "QNX ", 4));
    CHECK(pread(fd, buf, 4, 4) == 4 && !memcmp(buf, "call", 4));
    CHECK(lseek(fd, 0, SEEK_CUR) == 4); /* pread did not move the shared offset. */
    CHECK(read(fd, buf, sizeof(buf)) == 9 && !memcmp(buf, "callback!", 9));
    CHECK(read(fd, buf, sizeof(buf)) == 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    int copy = dup(fd); CHECK(copy >= 0);
    CHECK(read(copy, buf, 4) == 4);
    CHECK(lseek(fd, 0, SEEK_CUR) == 4);
    CHECK(fstat(fd, &st) == 0 && st.st_size == 13);
    CHECK(close(copy) == 0 && close(fd) == 0);
    errno = 0; CHECK(open("/qfuse-hello/greeting", O_WRONLY) == -1 && errno == EROFS);
    errno = 0; CHECK(chmod("/qfuse-hello/greeting", 0777) == -1);
    CHECK(stat("/qfuse-hello/greeting", &st) == 0 && (st.st_mode & 0777) == 0444);
    CHECK(system("ls -la /qfuse-hello") == 0);
    CHECK(system("stat /qfuse-hello/greeting") == 0);
    CHECK(system("cat /qfuse-hello/greeting") == 0);
    puts("");
    puts("PASS native mount: stat, readdir, rewind, read, pread, EOF, seek, dup, fstat, read-only errors, ls, cat");
}

int main(int argc, char **argv)
{
    struct sigaction action = {0};
    action.sa_handler = timeout_test;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGALRM, &action, NULL) == 0);
    alarm(90); /* Bound a hung filesystem call in the disposable test target. */
    atexit(stop_server);
    if (argc == 2 && !strcmp(argv[1], "--security-only")) {
        check_context_and_permissions(); alarm(0); return 0;
    }
    CHECK(argc == 1);
    start_server(0); check_mount(); stop_server();
    struct stat st;
    CHECK(stat("/qfuse-hello/greeting", &st) == -1);
    start_server(0); check_mount(); stop_server();
    puts("PASS native mount: graceful termination, detached path, remount");
    check_context_and_permissions();
    alarm(0);
    return 0;
}
