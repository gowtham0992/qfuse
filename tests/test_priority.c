/* SPDX-License-Identifier: MIT */
#include <sys/stat.h>
#include <sys/wait.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
struct priority_event {
    int priority;
    pid_t caller;
};
static pid_t server, client;
static int resume_write = -1;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL priority line %d: %s errno=%d\n", __LINE__, #x, errno);          \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static void stop(void)
{
    if (resume_write >= 0) {
        close(resume_write);
        resume_write = -1;
    }
    if (client > 0) {
        kill(client, SIGKILL);
        waitpid(client, NULL, 0);
        client = 0;
    }
    if (server > 0) {
        kill(server, SIGTERM);
        waitpid(server, NULL, 0);
        server = 0;
    }
}
static void timeout(int n)
{
    (void)n;
    if (client > 0)
        kill(client, SIGKILL);
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
    int notify[2], resume[2];
    CHECK(pipe(notify) == 0 && pipe(resume) == 0);
    server = fork();
    CHECK(server >= 0);
    if (!server) {
        close(notify[0]);
        close(resume[1]);
        char a[32], b[32];
        snprintf(a, sizeof(a), "%d", notify[1]);
        snprintf(b, sizeof(b), "%d", resume[0]);
        setenv("QFUSE_PRIORITY_NOTIFY_FD", a, 1);
        setenv("QFUSE_PRIORITY_RESUME_FD", b, 1);
        execl("/data/qfuse/qfuse-priority", "priority-fs", "-f", "/qfuse-priority", NULL);
        _exit(127);
    }
    close(notify[1]);
    close(resume[0]);
    resume_write = resume[1];
    for (int i = 0; i < 200; i++) {
        struct stat st;
        if (!stat("/qfuse-priority/priority", &st))
            break;
        CHECK(i < 199);
        struct timespec p = {0, 100000000};
        nanosleep(&p, NULL);
    }
    for (int priority = 20; priority <= 30; priority += 10) {
        client = fork();
        CHECK(client >= 0);
        if (!client) {
            close(notify[0]);
            close(resume[1]);
            struct sched_param p = {.sched_priority = priority};
            CHECK(pthread_setschedparam(pthread_self(), SCHED_RR, &p) == 0);
            int fd = open("/qfuse-priority/priority", O_RDONLY);
            CHECK(fd >= 0);
            char value[64] = {0};
            CHECK(read(fd, value, sizeof(value) - 1) > 0);
            int observed;
            long caller;
            CHECK(sscanf(value, "%d %ld", &observed, &caller) == 2 && observed == priority &&
                  caller == getpid());
            CHECK(close(fd) == 0);
            _exit(0);
        }
        struct priority_event event;
        CHECK(read(notify[0], &event, sizeof(event)) == sizeof(event));
        CHECK(event.priority == priority && event.caller == client);
        printf("PASS native priority inheritance: client priority %d reached the callback "
               "unchanged, with authenticated caller PID\n",
               priority);
        if (priority == 30) {
            pid_t inspection = fork();
            CHECK(inspection >= 0);
            if (!inspection) {
                char pid[32];
                snprintf(pid, sizeof(pid), "%ld", (long)server);
                execlp("pidin", "pidin", "-p", pid, "-F", "%a %b %28n %p %J %B", NULL);
                _exit(127);
            }
            int status;
            CHECK(waitpid(inspection, &status, 0) == inspection && WIFEXITED(status) &&
                  !WEXITSTATUS(status));
        }
        CHECK(write(resume[1], "G", 1) == 1);
        int status;
        CHECK(waitpid(client, &status, 0) == client && WIFEXITED(status) && !WEXITSTATUS(status));
        client = 0;
    }
    close(notify[0]);
    close(resume[1]);
    resume_write = -1;
    stop();
    alarm(0);
    puts("PASS QNX IPC priority and callback context evidence");
    return 0;
}
