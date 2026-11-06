/* SPDX-License-Identifier: MIT */
#include <sys/stat.h>
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
            fprintf(stderr, "FAIL write permissions line %d: %s errno=%d (%s)\n", __LINE__, #x,    \
                    errno, strerror(errno));                                                       \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static void stop(void)
{
    if (server > 0) {
        kill(server, SIGTERM);
        waitpid(server, NULL, 0);
        server = 0;
    }
}
static void timeout(int n)
{
    (void)n;
    if (server > 0)
        kill(server, SIGKILL);
    _exit(124);
}
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGALRM, timeout);
    alarm(120);
    atexit(stop);
    char fixture[] = "/data/qfuse-permissions.XXXXXX";
    CHECK(mkdtemp(fixture) && chmod(fixture, 0755) == 0);
    char shared[512], private[512], victim[512], userfile[1024], denied[1024], protected[1024];
    snprintf(shared, sizeof(shared), "%s/shared", fixture);
    snprintf(private, sizeof(private), "%s/private", fixture);
    snprintf(victim, sizeof(victim), "%s/shared/root-file", fixture);
    CHECK(mkdir(shared, 0777) == 0 && chmod(shared, 01777) == 0 && mkdir(private, 0700) == 0);
    int fd = open(victim, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    server = fork();
    CHECK(server >= 0);
    if (!server) {
        execl("/data/qfuse/qfuse-passthrough", "passthrough", "-f", "/qfuse-permissions", NULL);
        _exit(127);
    }
    snprintf(userfile, sizeof(userfile), "/qfuse-permissions%s/shared/by-user", fixture);
    snprintf(denied, sizeof(denied), "/qfuse-permissions%s/private/denied", fixture);
    snprintf(protected, sizeof(protected), "/qfuse-permissions%s/shared/root-file", fixture);
    for (int i = 0; i < 200; i++) {
        struct stat st;
        if (!stat(protected, &st))
            break;
        CHECK(i < 199);
        struct timespec p = {0, 100000000};
        nanosleep(&p, NULL);
    }
    pid_t client = fork();
    CHECK(client >= 0);
    if (!client) {
        CHECK(setgid(21003) == 0 && setuid(21003) == 0);
        umask(0027);
        fd = open(userfile, O_CREAT | O_EXCL | O_RDWR, 0666);
        CHECK(fd >= 0);
        struct stat st;
        CHECK(fstat(fd, &st) == 0 && st.st_uid == 21003 && st.st_gid == 21003 &&
              (st.st_mode & 0777) == 0640);
        CHECK(write(fd, "user", 4) == 4 && fchmod(fd, 0600) == 0 && close(fd) == 0);
        errno = 0;
        CHECK(open(denied, O_CREAT | O_WRONLY, 0600) == -1 && errno == EACCES);
        errno = 0;
        CHECK(unlink(protected) == -1 && errno == EPERM);
        errno = 0;
        CHECK(chmod(protected, 0666) == -1 && (errno == EPERM || errno == EACCES));
        CHECK(unlink(userfile) == 0);
        _exit(0);
    }
    int status;
    CHECK(waitpid(client, &status, 0) == client && WIFEXITED(status) && !WEXITSTATUS(status));
    stop();
    CHECK(unlink(victim) == 0 && rmdir(shared) == 0 && rmdir(private) == 0 && rmdir(fixture) == 0);
    alarm(0);
    puts("PASS writable security: authenticated owner/group, caller umask, private-parent denial, "
         "sticky unlink and chmod denial");
    return 0;
}
