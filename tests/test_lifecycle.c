/* SPDX-License-Identifier: MIT */
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/neutrino.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static pid_t server, copier;
static char fixture[512], mounted[1024];
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL lifecycle line %d: %s errno=%d (%s)\n", __LINE__, #x, errno,     \
                    strerror(errno));                                                              \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static void timeout(int s)
{
    (void)s;
    if (server > 0)
        kill(server, SIGKILL);
    if (copier > 0)
        kill(copier, SIGKILL);
    _exit(124);
}
static void stop(void)
{
    if (server > 0) {
        kill(server, SIGTERM);
        waitpid(server, NULL, 0);
        server = 0;
    }
}
static void start(void)
{
    server = fork();
    CHECK(server >= 0);
    if (!server) {
        execl(getenv("QFUSE_PASSTHROUGH_BINARY") ? getenv("QFUSE_PASSTHROUGH_BINARY")
                                                 : "/data/qfuse/qfuse-passthrough",
              "passthrough", "-f", "-o",
              getenv("QFUSE_LIFECYCLE_MODE") ? getenv("QFUSE_LIFECYCLE_MODE") : "ro",
              "/qfuse-lifecycle", NULL);
        _exit(127);
    }
    for (int i = 0; i < 200; i++) {
        struct stat st;
        if (!stat(mounted, &st))
            return;
        int status;
        if (waitpid(server, &status, WNOHANG) == server) {
            server = 0;
            CHECK(!"early server exit");
        }
        struct timespec t = {0, 100000000};
        nanosleep(&t, NULL);
    }
    CHECK(!"mount readiness");
}
static int command(const char *binary, const char *arg)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        execl(binary, binary, arg, NULL);
        perror("exec mounted command");
        _exit(127);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status));
    return WEXITSTATUS(status);
}
static int force_unmount(const char *path)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        execl("/data/qfuse/qfuse-unmount", "qfuse-unmount", "-f", path, NULL);
        _exit(127);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status));
    return WEXITSTATUS(status);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGALRM, timeout);
    alarm(180);
    atexit(stop);
    strcpy(fixture, "/data/qfuse-lifecycle.XXXXXX");
    CHECK(mkdtemp(fixture));
    char path[600];
    snprintf(path, sizeof(path), "%s/probe", fixture);
    int source = open("/data/qfuse/exec-probe", O_RDONLY),
        out = open(path, O_CREAT | O_EXCL | O_WRONLY, 0755);
    CHECK(source >= 0 && out >= 0);
    char buffer[8192];
    ssize_t n;
    while ((n = read(source, buffer, sizeof(buffer))) > 0)
        CHECK(write(out, buffer, n) == n);
    CHECK(n == 0 && close(source) == 0 && close(out) == 0);
    snprintf(mounted, sizeof(mounted), "/qfuse-lifecycle%s/probe", fixture);
    start();
    int execute_only = open(mounted, O_EXEC);
    CHECK(execute_only >= 0);
    errno = 0;
    CHECK(read(execute_only, buffer, 1) == -1 && errno == EBADF);
    CHECK(close(execute_only) == 0);
    /* The unmount helper is exec'd below. It must not inherit the descriptor
       whose lifetime this test controls; process-exit cleanup is asynchronous. */
    int fd = open(mounted, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(command("/data/qfuse/qfuse-unmount", "/qfuse-lifecycle") == 1);
    CHECK(close(fd) == 0);
    CHECK(command("/data/qfuse/qfuse-unmount", "/qfuse-lifecycle") == 0);
    int status;
    CHECK(waitpid(server, &status, 0) == server && WIFEXITED(status) && !WEXITSTATUS(status));
    server = 0;
    puts("PASS unmount: busy mount rejected, idle mount detached, server exited cleanly");
    start();
    CHECK(command(mounted, NULL) == 37);
    puts("PASS executable loading: QNX loader executed the binary through the mount");
    /* QNX may retain a real pager OCB in its image cache after the process exits.
       Normal busy checks remain strict; force is an explicit native operation. */
    fd = open(mounted, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(force_unmount("/qfuse-lifecycle") == 0);
    CHECK(waitpid(server, &status, 0) == server && WIFEXITED(status) && !WEXITSTATUS(status));
    server = 0;
    uint64_t forced_limit = 1000000000ULL;
    CHECK(TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &forced_limit,
                       NULL) == 0);
    errno = 0;
    CHECK(read(fd, buffer, 1) == -1 && errno != ETIMEDOUT);
    close(fd);
    puts("PASS explicit forced unmount: pager/user handles detached and active client receives a "
         "native error");
    start();
    int ready[2], resume[2];
    CHECK(pipe(ready) == 0 && pipe(resume) == 0);
    char partial[600];
    snprintf(partial, sizeof(partial), "%s/partial-copy", fixture);
    copier = fork();
    CHECK(copier >= 0);
    if (!copier) {
        close(ready[0]);
        close(resume[1]);
        fd = open(mounted, O_RDONLY);
        int output = open(partial, O_CREAT | O_EXCL | O_WRONLY, 0600);
        CHECK(fd >= 0 && output >= 0);
        CHECK(read(fd, buffer, 4096) == 4096 && write(output, buffer, 4096) == 4096);
        CHECK(write(ready[1], "R", 1) == 1);
        char go;
        CHECK(read(resume[0], &go, 1) == 1);
        uint64_t limit = 1000000000ULL;
        CHECK(TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &limit,
                           NULL) == 0);
        errno = 0;
        CHECK(read(fd, buffer, sizeof(buffer)) == -1 && errno != ETIMEDOUT);
        close(fd);
        CHECK(close(output) == 0);
        _exit(0);
    }
    close(ready[1]);
    close(resume[0]);
    char announced;
    CHECK(read(ready[0], &announced, 1) == 1);
    close(ready[0]);
    if (!getenv("QFUSE_NEGATIVE_LEAVE_SERVER_ALIVE")) {
        CHECK(kill(server, SIGKILL) == 0 && waitpid(server, &status, 0) == server);
        server = 0;
    }
    CHECK(write(resume[1], "G", 1) == 1);
    close(resume[1]);
    CHECK(waitpid(copier, &status, 0) == copier && WIFEXITED(status) && !WEXITSTATUS(status));
    copier = 0;
    struct stat copied;
    CHECK(stat(partial, &copied) == 0 && copied.st_size == 4096 && unlink(partial) == 0);
    puts("PASS daemon death during copy: partial copy rejected with a native error instead of "
         "hanging or false EOF");
    start();
    CHECK(command(mounted, NULL) == 37);
    stop();
    CHECK(unlink(path) == 0 && rmdir(fixture) == 0);
    alarm(0);
    puts("PASS recovery: remounted after daemon death and executed the same binary");
    return 0;
}
