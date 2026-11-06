/* SPDX-License-Identifier: MIT */
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
static pid_t children[2];
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL mount identity line %d: %s errno=%d\n", __LINE__, #x, errno);    \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static void stop(void)
{
    for (int i = 0; i < 2; i++)
        if (children[i] > 0) {
            kill(children[i], SIGTERM);
            waitpid(children[i], NULL, 0);
            children[i] = 0;
        }
}
static void timeout(int n)
{
    (void)n;
    for (int i = 0; i < 2; i++)
        if (children[i] > 0)
            kill(children[i], SIGKILL);
    _exit(124);
}
int main(void)
{
    signal(SIGALRM, timeout);
    alarm(90);
    atexit(stop);
    struct stat st[2];
    const char *mounts[] = {"/qfuse-identity-a", "/qfuse-identity-b"};
    const char *files[] = {"/qfuse-identity-a/identity", "/qfuse-identity-b/identity"};
    for (int i = 0; i < 2; i++) {
        children[i] = fork();
        CHECK(children[i] >= 0);
        if (!children[i]) {
            execl("/data/qfuse/qfuse-hello", "hello", "-f", "--name=identity",
                  i ? "--contents=B" : "--contents=A", mounts[i], NULL);
            _exit(127);
        }
        for (int j = 0; j < 200; j++) {
            if (!stat(files[i], &st[i]))
                break;
            CHECK(j < 199);
            struct timespec p = {0, 100000000};
            nanosleep(&p, NULL);
        }
    }
    fprintf(stdout, "Mounted filesystem devices: %llu and %llu\n", (unsigned long long)st[0].st_dev,
            (unsigned long long)st[1].st_dev);
    fflush(stdout);
    CHECK(st[0].st_dev != st[1].st_dev);
    for (int i = 0; i < 2; i++) {
        int fd = open(files[i], O_RDONLY);
        char c;
        CHECK(fd >= 0 && read(fd, &c, 1) == 1 && c == (i ? 'B' : 'A') && close(fd) == 0);
    }
    stop();
    alarm(0);
    puts("PASS distinct filesystem identities for simultaneous native resource-manager mounts");
    return 0;
}
