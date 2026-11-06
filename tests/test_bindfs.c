/* SPDX-License-Identifier: MIT — native checks for the optional real bindfs port. */
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL bindfs line %d: %s (%s)\n", __LINE__, #x, strerror(errno)); exit(1); } } while (0)
static char folder[] = "/data/qfuse-bind.XXXXXX", source[512], view[512];
static int mounted;
static int command(char *const args[])
{
    pid_t p = fork(); CHECK(p >= 0);
    if (!p) { execv(args[0], args); _exit(127); }
    int status; CHECK(waitpid(p, &status, 0) == p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}
static void stop(void)
{
    if (!mounted) return;
    char *args[] = {"/data/qfuse/qfuse-mount", "stop", "--force", view, NULL};
    (void)command(args); mounted = 0;
}
static void start(int mask)
{
    char *args[] = {"/data/qfuse/qfuse-mount", "start", view, "--", "/data/qfuse/bindfs", "--multithreaded", "-f", mask ? "--perms=a-w" : "--perms=u+rw", source, view, NULL};
    CHECK(command(args) == 0); mounted = 1;
}
int main(void)
{
    CHECK(mkdtemp(folder));
    snprintf(source, sizeof(source), "%s/source", folder);
    snprintf(view, sizeof(view), "%s/view", folder);
    CHECK(mkdir(source, 0700) == 0 && mkdir(view, 0700) == 0);
    atexit(stop);
    char original[600], alias[600], linkpath[600];
    snprintf(original, sizeof(original), "%s/hello", source);
    snprintf(alias, sizeof(alias), "%s/hello", view);
    snprintf(linkpath, sizeof(linkpath), "%s/link", view);
    int fd = open(original, O_CREAT | O_EXCL | O_WRONLY, 0644); CHECK(fd >= 0);
    CHECK(write(fd, "first", 5) == 5 && close(fd) == 0 && chmod(original, 0644) == 0);
    start(0);
    fd = open(alias, O_WRONLY | O_TRUNC); CHECK(fd >= 0);
    CHECK(write(fd, "edited", 6) == 6 && close(fd) == 0);
    CHECK(symlink("hello", linkpath) == 0);
    char bytes[16] = {0}; fd = open(linkpath, O_RDONLY); CHECK(fd >= 0);
    CHECK(read(fd, bytes, sizeof(bytes)) == 6 && !memcmp(bytes, "edited", 6) && close(fd) == 0);
    stop();
    fd = open(original, O_RDONLY); CHECK(fd >= 0); memset(bytes, 0, sizeof(bytes));
    CHECK(read(fd, bytes, sizeof(bytes)) == 6 && !memcmp(bytes, "edited", 6) && close(fd) == 0);
    start(1);
    struct stat backing, mapped; CHECK(stat(original, &backing) == 0 && stat(alias, &mapped) == 0);
    CHECK((backing.st_mode & 0777) == 0644 && (mapped.st_mode & 0777) == 0444);
    CHECK(backing.st_ino == mapped.st_ino);
    stop();
    snprintf(linkpath, sizeof(linkpath), "%s/link", source);
    CHECK(unlink(linkpath) == 0 && unlink(original) == 0);
    CHECK(rmdir(view) == 0 && rmdir(source) == 0 && rmdir(folder) == 0);
    puts("PASS native bindfs: write-through, symlink/read, persistence, permission transformation, use_ino and remount");
    return 0;
}
