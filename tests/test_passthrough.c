/* SPDX-License-Identifier: MIT
 * Independent client: compare a real QNX tree with its passthrough view.
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

#define FILE_COUNT 5000
static volatile sig_atomic_t server_pid;
static char fixture[256];
static char mounted[512];
static void cleanup(void);
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL passthrough line %d: %s (errno=%d: %s)\n", __LINE__, #condition, errno, strerror(errno)); \
    exit(1); } } while (0)

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
    for (int i = 0; i < 200; i++) {
        int status;
        if (waitpid(server_pid, &status, WNOHANG) == server_pid) {
            server_pid = 0;
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            return;
        }
        struct timespec pause = {0, 100000000}; nanosleep(&pause, NULL);
    }
    kill(server_pid, SIGKILL); waitpid(server_pid, NULL, 0); server_pid = 0;
    CHECK(!"passthrough server required SIGKILL");
}
static void remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st)) return;
    if (!S_ISDIR(st.st_mode)) { unlink(path); return; }
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[1024];
        int length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (length > 0 && (size_t)length < sizeof(child)) remove_tree(child);
    }
    closedir(dir); rmdir(path);
}
static void cleanup(void)
{
    stop_server();
    if (fixture[0]) remove_tree(fixture); /* Only this test's mkdtemp tree. */
}
static void path_for(char *out, size_t size, const char *root, const char *suffix)
{
    int n = snprintf(out, size, "%s/%s", root, suffix);
    CHECK(n > 0 && (size_t)n < size);
}
static void write_file(const char *path, size_t size, unsigned seed)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); CHECK(fd >= 0);
    unsigned char data[4096];
    size_t position = 0;
    while (position < size) {
        size_t length = size - position;
        if (length > sizeof(data)) length = sizeof(data);
        for (size_t i = 0; i < length; i++) data[i] = (unsigned char)((position + i) * 37u + seed * 19u);
        CHECK(write(fd, data, length) == (ssize_t)length);
        position += length;
    }
    CHECK(close(fd) == 0);
}
static void compare_handles(int native, int view)
{
    unsigned char a[8192], b[8192];
    for (;;) {
        ssize_t an = read(native, a, sizeof(a)); CHECK(an >= 0);
        ssize_t bn = read(view, b, sizeof(b)); CHECK(bn >= 0);
        CHECK(an == bn && !memcmp(a, b, an));
        if (!an) break;
    }
}
static void compare_file(const char *suffix)
{
    char source[512], view[768];
    path_for(source, sizeof(source), fixture, suffix);
    path_for(view, sizeof(view), mounted, suffix);
    struct stat a, b;
    CHECK(stat(source, &a) == 0 && stat(view, &b) == 0);
    CHECK(a.st_size == b.st_size && a.st_mode == b.st_mode && a.st_ino == b.st_ino);
    CHECK(a.st_uid == b.st_uid && a.st_gid == b.st_gid && a.st_nlink == b.st_nlink);
    CHECK(a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec);
    int native = open(source, O_RDONLY); CHECK(native >= 0);
    int alias = open(view, O_RDONLY); CHECK(alias >= 0);
    compare_handles(native, alias);
    CHECK(close(native) == 0 && close(alias) == 0);
}
static void run_command(char *const args[])
{
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { execvp(args[0], args); _exit(127); }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void start_server(void)
{
    server_pid = fork(); CHECK(server_pid >= 0);
    if (!server_pid) {
        const char *binary = getenv("QFUSE_PASSTHROUGH_BINARY");
        execl(binary ? binary : "/usr/bin/qfuse-passthrough", "qfuse-passthrough", "-f", "-o", "ro", "/qfuse-pass", NULL);
        _exit(127);
    }
    char probe[768]; path_for(probe, sizeof(probe), mounted, "empty");
    for (int i = 0; i < 200; i++) {
        struct stat st;
        if (!stat(probe, &st)) return;
        int status; pid_t observed = waitpid(server_pid, &status, WNOHANG);
        if (observed == server_pid) {
            server_pid = 0;
            fprintf(stderr, "Server exited before readiness: status=%d\n", status);
            CHECK(!"server exited before readiness");
        }
        CHECK(observed == 0);
        struct timespec pause = {0, 100000000}; nanosleep(&pause, NULL);
    }
    CHECK(!"passthrough mount did not become ready");
}
static void check_read_only(void)
{
    char source[512], view[768], missing[768];
    path_for(source, sizeof(source), fixture, "nested/deep/binary");
    path_for(view, sizeof(view), mounted, "nested/deep/binary");
    path_for(missing, sizeof(missing), mounted, "new-file");
    errno = 0; CHECK(open(view, O_WRONLY) == -1 && errno == EROFS);
    errno = 0; CHECK(open(view, O_RDONLY | O_TRUNC) == -1 && errno == EROFS);
    errno = 0; CHECK(open(view, O_RDONLY | O_CREAT, 0644) == -1 && errno == EROFS);
    errno = 0; CHECK(open(missing, O_RDONLY | O_CREAT, 0644) == -1 && errno == EROFS);
    CHECK(chmod(view, 0777) == -1);
    CHECK(unlink(view) == -1);
    struct stat st; CHECK(stat(source, &st) == 0 && st.st_size == 200037 && (st.st_mode & 0777) == 0644);
    path_for(source, sizeof(source), fixture, "new-file");
    CHECK(lstat(source, &st) == -1 && errno == ENOENT);
    compare_file("nested/deep/binary");
    puts("PASS passthrough: create/truncate/write/chmod/unlink cannot change backing files");
}
static void check_directory(void)
{
    char path[768]; path_for(path, sizeof(path), mounted, "wide");
    DIR *dir = opendir(path); CHECK(dir);
    unsigned char seen[FILE_COUNT] = {0};
    int count = 0;
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        int number = -1, length = 0;
        CHECK(sscanf(entry->d_name, "file-%d.bin%n", &number, &length) == 1);
        CHECK(entry->d_name[length] == 0 && number >= 0 && number < FILE_COUNT && !seen[number]);
        seen[number] = 1; count++;
        if (number == 0 || number == FILE_COUNT - 1) {
            char child[1024]; path_for(child, sizeof(child), path, entry->d_name);
            struct stat st; CHECK(stat(child, &st) == 0 && st.st_ino == entry->d_ino);
        }
        errno = 0;
    }
    CHECK(errno == 0 && count == FILE_COUNT);
    rewinddir(dir); count = 0;
    while ((entry = readdir(dir))) if (entry->d_name[0] != '.') count++;
    CHECK(count == FILE_COUNT); CHECK(closedir(dir) == 0);
    puts("PASS passthrough: 5000-entry directory, continuation, rewind, unique names and inode consistency");
}
static void check_handle_lifetime(void)
{
    char source[512], moved[512], view[768];
    path_for(source, sizeof(source), fixture, "nested/deep/binary");
    path_for(moved, sizeof(moved), fixture, "nested/deep/original");
    path_for(view, sizeof(view), mounted, "nested/deep/binary");
    int native = open(source, O_RDONLY), alias = open(view, O_RDONLY);
    CHECK(native >= 0 && alias >= 0);
    CHECK(rename(source, moved) == 0);
    write_file(source, 111, 77);
    compare_handles(native, alias); /* Existing fh still reads the original inode. */
    CHECK(close(native) == 0 && close(alias) == 0);
    compare_file("nested/deep/binary");
    puts("PASS passthrough: existing read handle survives backing rename and replacement");
}
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction action = {0}; action.sa_handler = timeout_test;
    sigemptyset(&action.sa_mask); CHECK(sigaction(SIGALRM, &action, NULL) == 0);
    alarm(900);
    atexit(cleanup);
    strcpy(fixture, "/data/qfuse-pass.XXXXXX");
    CHECK(mkdtemp(fixture));
    int n = snprintf(mounted, sizeof(mounted), "/qfuse-pass%s", fixture);
    CHECK(n > 0 && (size_t)n < sizeof(mounted));
    char path[768];
    const char *dirs[] = {"nested", "nested/deep", "wide"};
    for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) {
        path_for(path, sizeof(path), fixture, dirs[i]); CHECK(mkdir(path, 0755) == 0);
    }
    path_for(path, sizeof(path), fixture, "empty"); write_file(path, 0, 0);
    path_for(path, sizeof(path), fixture, "nested/deep/binary"); write_file(path, 200037, 42);
    puts("Preparing 5000-file synthetic QNX fixture..."); fflush(stdout);
    for (int i = 0; i < FILE_COUNT; i++) {
        char name[64]; snprintf(name, sizeof(name), "wide/file-%04d.bin", i);
        path_for(path, sizeof(path), fixture, name); write_file(path, 97, i);
        if ((i + 1) % 1000 == 0) printf("Prepared %d synthetic files\n", i + 1);
    }
    start_server();
    compare_file("empty"); compare_file("nested/deep/binary");
    check_read_only(); check_directory();
    char source[768], copy[768], view[768];
    path_for(source, sizeof(source), fixture, "wide");
    path_for(copy, sizeof(copy), fixture, "copy");
    path_for(view, sizeof(view), mounted, "wide");
    char *cp_args[] = {"cp", "-R", view, copy, NULL}; run_command(cp_args);
    char *diff_args[] = {"diff", "-r", source, copy, NULL}; run_command(diff_args);
    puts("PASS passthrough: native cp copied 5000 files through the mount; native diff -r is clean");
    check_handle_lifetime();
    stop_server(); start_server(); compare_file("nested/deep/binary"); stop_server();
    puts("PASS passthrough: graceful detach and remount with updated backing contents");
    cleanup(); fixture[0] = 0;
    alarm(0);
    puts("PASS read-only passthrough suite");
    return 0;
}
