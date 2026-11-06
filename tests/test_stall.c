/* SPDX-License-Identifier: MIT
 * Functional isolation/unblock gates; no benchmark or general RT guarantee.
 */
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/neutrino.h>
#include <dirent.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "inflight_protocol.h"
struct outcome {
    _Atomic int done;
    int result, error;
    uint64_t finished;
};
struct state {
    struct outcome unrelated, priority, first, second;
    _Atomic uint64_t armed;
};
static struct state *state;
static pid_t server, reader, other;
static int notice[2], release_hold[2];
static char root[256], source[512], view[768];
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL stall line %d: %s errno=%d\n", __LINE__, #x, errno);             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static uint64_t now(void)
{
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
static void pause_short(void)
{
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}
static void signal_client(int n)
{
    (void)n;
}
static void timeout_test(int n)
{
    (void)n;
    if (server > 0)
        kill(server, SIGKILL);
    if (reader > 0)
        kill(reader, SIGKILL);
    if (other > 0)
        kill(other, SIGKILL);
    _exit(124);
}
static void cleanup(void)
{
    if (server > 0) {
        kill(server, SIGKILL);
        waitpid(server, NULL, 0);
        server = 0;
    }
    if (reader > 0) {
        kill(reader, SIGKILL);
        waitpid(reader, NULL, 0);
        reader = 0;
    }
    if (other > 0) {
        kill(other, SIGKILL);
        waitpid(other, NULL, 0);
        other = 0;
    }
    if (source[0])
        unlink(source);
    if (root[0])
        rmdir(root);
}
static int await(struct outcome *o, uint64_t end)
{
    while (!atomic_load(&o->done) && now() < end)
        pause_short();
    return atomic_load(&o->done);
}
static int event(struct qf_inflight_event *e, uint64_t end)
{
    while (now() < end) {
        ssize_t n = read(notice[0], e, sizeof(*e));
        if (n == (ssize_t)sizeof(*e))
            return 1;
        CHECK(n < 0 && (errno == EAGAIN || errno == EINTR));
        pause_short();
    }
    return 0;
}
static void publish(struct outcome *o, int result, int error)
{
    o->result = result;
    o->error = error;
    o->finished = now();
    atomic_store(&o->done, 1);
}
static void unrelated_client(struct outcome *o, int priority)
{
    if (priority) {
        struct sched_param p = {.sched_priority = priority};
        if (sched_setscheduler(0, SCHED_RR, &p) == -1)
            _exit(9);
    }
    DIR *d = opendir("/qfuse-stall/data/qfuse");
    int e = errno, count = 0;
    if (d) {
        errno = 0;
        while (readdir(d))
            count++;
        e = errno;
        if (closedir(d) && !e)
            e = errno;
    }
    int r = d && count > 0 && !e ? 0 : -1;
    publish(o, r, e);
    _exit(r ? 1 : 0);
}
int main(int argc, char **argv)
{
    int timer = argc == 2 && !strcmp(argv[1], "--timer");
    int exiting = argc == 2 && !strcmp(argv[1], "--exit");
    CHECK(argc == 1 || timer || exiting);
    setvbuf(stdout, NULL, _IOLBF, 0);
    atexit(cleanup);
    signal(SIGALRM, timeout_test);
    alarm(60);
    state = mmap(NULL, sizeof(*state), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    CHECK(state != MAP_FAILED);
    memset(state, 0, sizeof(*state));
    strcpy(root, "/data/qfuse-stall.XXXXXX");
    CHECK(mkdtemp(root));
    snprintf(source, sizeof(source), "%s/big", root);
    snprintf(view, sizeof(view), "/qfuse-stall%s", source);
    int fd = open(source, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(fd >= 0);
    char data[8192];
    memset(data, 'Q', sizeof(data));
    CHECK(write(fd, data, sizeof(data)) == sizeof(data));
    CHECK(!close(fd));
    CHECK(!pipe(notice) && !pipe(release_hold));
    CHECK(fcntl(notice[0], F_SETFL, O_NONBLOCK) != -1);
    server = fork();
    CHECK(server >= 0);
    if (!server) {
        char a[32], b[32];
        snprintf(a, sizeof(a), "%d", notice[1]);
        snprintf(b, sizeof(b), "%d", release_hold[0]);
        setenv("QFUSE_INFLIGHT_NOTIFY_FD", a, 1);
        setenv("QFUSE_INFLIGHT_RELEASE_FD", b, 1);
        setenv("QFUSE_INFLIGHT_PATH", source, 1);
        execl("/data/qfuse/qfuse-inflight", "inflight", "-f", "-o", "ro", "/qfuse-stall", NULL);
        _exit(127);
    }
    for (int i = 0; i < 200; i++) {
        struct stat s;
        if (!stat(view, &s))
            break;
        CHECK(i < 199);
        struct timespec t = {0, 100000000};
        nanosleep(&t, NULL);
    }
    reader = fork();
    CHECK(reader >= 0);
    if (!reader) {
        struct sigaction s = {0};
        s.sa_handler = signal_client;
        sigemptyset(&s.sa_mask);
        if (sigaction(SIGUSR1, &s, NULL))
            _exit(2);
        int f = open(view, O_RDONLY);
        if (f < 0)
            _exit(3);
        char b[4096];
        if (timer) {
            uint64_t ns = 250000000;
            atomic_store(&state->armed, now());
            if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &ns,
                             NULL))
                _exit(8);
        }
        errno = 0;
        int n = pread(f, b, sizeof(b), 4096);
        int saved = errno;
        publish(&state->first, n, saved);
        if (n != -1 || saved != (timer ? ETIMEDOUT : EINTR))
            _exit(4);
        if (exiting) {
            int closed = close(f);
            publish(&state->second, closed, errno);
            _exit(closed ? 6 : 0);
        }
        errno = 0;
        n = pread(f, b, sizeof(b), 4096);
        saved = errno;
        if (n == 4096)
            for (size_t i = 0; i < sizeof(b); i++)
                if (b[i] != 'Q')
                    n = -2;
        publish(&state->second, n, saved);
        close(f);
        _exit(n == 4096 ? 0 : 5);
    }
    struct qf_inflight_event e;
    CHECK(event(&e, now() + 10000000000ULL));
    CHECK(e.caller == reader && e.offset == 4096);
    puts("PASS stall precondition: read entered held upstream callback");
    other = fork();
    CHECK(other >= 0);
    if (!other)
        unrelated_client(&state->unrelated, 0);
    uint64_t started = now();
    int independent =
        await(&state->unrelated, started + 1000000000ULL) && state->unrelated.result == 0;
    printf("%s unrelated path completes while read remains held\n", independent ? "PASS" : "FAIL");
    int high = 0, status;
    if (independent) {
        CHECK(waitpid(other, &status, 0) == other && WIFEXITED(status) && !WEXITSTATUS(status));
        other = 0;
        other = fork();
        CHECK(other >= 0);
        if (!other)
            unrelated_client(&state->priority, 50);
        high = await(&state->priority, now() + 1000000000ULL) && state->priority.result == 0;
    }
    printf("%s priority-50 directory listing completes while read remains held\n",
           high ? "PASS" : "FAIL");
    started = timer ? atomic_load(&state->armed) : now();
    if (!timer)
        CHECK(!kill(reader, SIGUSR1));
    int interrupted = await(&state->first, started + 1000000000ULL) && state->first.result == -1 &&
                      state->first.error == (timer ? ETIMEDOUT : EINTR) &&
                      state->first.finished - started < 1000000000ULL;
    printf("%s held client returns %s before callback release\n", interrupted ? "PASS" : "FAIL",
           timer ? "ETIMEDOUT" : "EINTR");
    int closed = 1;
    if (exiting) {
        uint64_t end = now() + 1000000000ULL;
        closed = await(&state->second, end) && state->second.result == 0;
        int exited = 0;
        while (closed && now() < end) {
            pid_t p = waitpid(reader, &status, WNOHANG);
            if (p == reader) {
                reader = 0;
                exited = WIFEXITED(status) && !WEXITSTATUS(status);
                break;
            }
            pause_short();
        }
        closed = closed && exited;
        printf("%s cancelled client closes and exits before callback release\n",
               closed ? "PASS" : "FAIL");
    }
    CHECK(write(release_hold[1], "x", 1) == 1);
    if (!exiting && independent && high && interrupted) {
        CHECK(event(&e, now() + 5000000000ULL));
        CHECK(e.caller == reader);
        CHECK(!atomic_load(&state->second.done));
        CHECK(write(release_hold[1], "x", 1) == 1);
        CHECK(await(&state->second, now() + 5000000000ULL));
        CHECK(state->second.result == 4096);
        puts("PASS late callback completion cannot reply to the client's next held request");
    }
    CHECK(independent && high && interrupted && closed);
    if (reader) {
        CHECK(waitpid(reader, &status, 0) == reader && WIFEXITED(status) && !WEXITSTATUS(status));
        reader = 0;
    }
    CHECK(waitpid(other, &status, 0) == other && WIFEXITED(status) && !WEXITSTATUS(status));
    other = 0;
    cleanup();
    alarm(0);
    puts("PASS held-callback isolation and client interruption");
    return 0;
}
