/* SPDX-License-Identifier: MIT */
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
static pid_t server, clients[8];
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL concurrency line %d: %s errno=%d (%s)\n", __LINE__, #x, errno,   \
                    strerror(errno));                                                              \
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
    for (int i = 0; i < 8; i++)
        if (clients[i] > 0)
            kill(clients[i], SIGKILL);
    _exit(124);
}
struct record {
    uint32_t magic, client, sequence, check;
};
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGALRM, timeout);
    alarm(120);
    atexit(stop);
    char fixture[] = "/data/qfuse-concurrency.XXXXXX", path[512], view[1024];
    CHECK(mkdtemp(fixture));
    snprintf(path, sizeof(path), "%s/log", fixture);
    snprintf(view, sizeof(view), "/qfuse-concurrency%s/log", fixture);
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    server = fork();
    CHECK(server >= 0);
    if (!server) {
        execl("/data/qfuse/qfuse-passthrough", "passthrough", "-f", "/qfuse-concurrency", NULL);
        _exit(127);
    }
    for (int i = 0; i < 200; i++) {
        struct stat st;
        if (!stat(view, &st))
            break;
        CHECK(i < 199);
        struct timespec p = {0, 100000000};
        nanosleep(&p, NULL);
    }
    for (int i = 0; i < 8; i++) {
        clients[i] = fork();
        CHECK(clients[i] >= 0);
        if (!clients[i]) {
            fd = open(view, O_WRONLY | O_APPEND);
            CHECK(fd >= 0);
            for (int j = 0; j < 16; j++) {
                struct record r = {0x51465553, (uint32_t)i, (uint32_t)j,
                                   0x51465553 ^ (uint32_t)i ^ (uint32_t)j};
                CHECK(write(fd, &r, sizeof(r)) == sizeof(r));
            }
            CHECK(close(fd) == 0);
            _exit(0);
        }
    }
    for (int i = 0; i < 8; i++) {
        int status;
        CHECK(waitpid(clients[i], &status, 0) == clients[i] && WIFEXITED(status) &&
              !WEXITSTATUS(status));
        clients[i] = 0;
    }
    unsigned seen[8][16] = {0};
    fd = open(view, O_RDONLY);
    CHECK(fd >= 0);
    for (int i = 0; i < 128; i++) {
        struct record r;
        CHECK(read(fd, &r, sizeof(r)) == sizeof(r));
        CHECK(r.magic == 0x51465553 && r.client < 8 && r.sequence < 16 &&
              r.check == (r.magic ^ r.client ^ r.sequence));
        CHECK(++seen[r.client][r.sequence] == 1);
    }
    char byte;
    CHECK(read(fd, &byte, 1) == 0 && close(fd) == 0);
    stop();
    CHECK(unlink(path) == 0 && rmdir(fixture) == 0);
    alarm(0);
    puts("PASS concurrent clients: 8 processes appended 128 intact unique records through the "
         "four-worker resource manager");
    return 0;
}
