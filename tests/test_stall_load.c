/* SPDX-License-Identifier: MIT
 * All dispatch workers may be occupied. Kernel unblock monitoring must still
 * service timeouts, without cancelling foreign code or releasing live handles.
 */
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/neutrino.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include "inflight_protocol.h"
#define CLIENTS 32
struct result {
    _Atomic int done;
    int returned, error;
    uint64_t deadline, finished;
};
static struct shared {
    _Atomic int ready, go;
    struct result result[CLIENTS];
} * shared;
static int file, notices[2], releases[2];
static pid_t server, client;
static char root[256], source[512];
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL saturation line %d: %s errno=%d\n", __LINE__, #x, errno);        \
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
static void cleanup(void)
{
    if (server > 0) {
        kill(server, SIGKILL);
        waitpid(server, NULL, 0);
        server = 0;
    }
    if (client > 0) {
        kill(client, SIGKILL);
        waitpid(client, NULL, 0);
        client = 0;
    }
    if (source[0])
        unlink(source);
    if (root[0])
        rmdir(root);
}
static void timeout_test(int n)
{
    (void)n;
    if (server > 0)
        kill(server, SIGKILL);
    if (client > 0)
        kill(client, SIGKILL);
    _exit(124);
}
static void *read_client(void *p)
{
    struct result *r = p;
    atomic_fetch_add(&shared->ready, 1);
    while (!atomic_load(&shared->go))
        pause_short();
    uint64_t duration = 15000000000ULL;
    r->deadline = now() + duration;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &duration,
                     NULL))
        _exit(8);
    char bytes[4096];
    errno = 0;
    r->returned = pread(file, bytes, sizeof(bytes), 4096);
    r->error = errno;
    r->finished = now();
    atomic_store(&r->done, 1);
    return NULL;
}
static unsigned reply_blocked(pid_t pid)
{
    int p[2];
    CHECK(!pipe(p));
    pid_t probe = fork();
    CHECK(probe >= 0);
    if (!probe) {
        close(p[0]);
        dup2(p[1], STDOUT_FILENO);
        close(p[1]);
        char number[32];
        snprintf(number, sizeof(number), "%d", pid);
        execlp("pidin", "pidin", "-p", number, NULL);
        _exit(127);
    }
    close(p[1]);
    FILE *stream = fdopen(p[0], "r");
    CHECK(stream);
    char line[512];
    unsigned count = 0;
    while (fgets(line, sizeof(line), stream))
        if (strstr(line, "REPLY"))
            count++;
    fclose(stream);
    int status;
    CHECK(waitpid(probe, &status, 0) == probe && WIFEXITED(status) && !WEXITSTATUS(status));
    return count;
}
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    atexit(cleanup);
    signal(SIGALRM, timeout_test);
    alarm(60);
    shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    CHECK(shared != MAP_FAILED);
    memset(shared, 0, sizeof(*shared));
    strcpy(root, "/data/qfuse-load.XXXXXX");
    CHECK(mkdtemp(root));
    snprintf(source, sizeof(source), "%s/big", root);
    int f = open(source, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(f >= 0);
    char bytes[8192];
    memset(bytes, 'Q', sizeof(bytes));
    CHECK(write(f, bytes, sizeof(bytes)) == sizeof(bytes));
    CHECK(!close(f));
    CHECK(!pipe(notices) && !pipe(releases));
    CHECK(fcntl(notices[0], F_SETFL, O_NONBLOCK) != -1);
    server = fork();
    CHECK(server >= 0);
    if (!server) {
        char a[32], b[32];
        snprintf(a, sizeof(a), "%d", notices[1]);
        snprintf(b, sizeof(b), "%d", releases[0]);
        setenv("QFUSE_INFLIGHT_NOTIFY_FD", a, 1);
        setenv("QFUSE_INFLIGHT_RELEASE_FD", b, 1);
        setenv("QFUSE_INFLIGHT_PATH", source, 1);
        const char *binary = getenv("QFUSE_INFLIGHT_BINARY");
        execl(binary ? binary : "/data/qfuse/qfuse-inflight", "inflight", "-f", "-o", "ro",
              "/qfuse-load", NULL);
        _exit(127);
    }
    char view[768];
    snprintf(view, sizeof(view), "/qfuse-load%s", source);
    for (int i = 0; i < 200; i++) {
        struct stat st;
        if (!stat(view, &st))
            break;
        CHECK(i < 199);
        struct timespec t = {0, 100000000};
        nanosleep(&t, NULL);
    }
    file = open(view, O_RDONLY);
    CHECK(file >= 0);
    client = fork();
    CHECK(client >= 0);
    if (!client) {
        char check;
        if (pread(file, &check, 1, 0) != 1 || check != 'Q')
            _exit(6);
        pthread_t threads[CLIENTS];
        for (int i = 0; i < CLIENTS; i++)
            if (pthread_create(&threads[i], NULL, read_client, &shared->result[i]))
                _exit(7);
        for (int i = 0; i < CLIENTS; i++)
            pthread_join(threads[i], NULL);
        close(file);
        _exit(0);
    }
    uint64_t end = now() + 10000000000ULL;
    while (atomic_load(&shared->ready) != CLIENTS && now() < end)
        pause_short();
    CHECK(atomic_load(&shared->ready) == CLIENTS);
    atomic_store(&shared->go, 1);
    struct qf_inflight_event event;
    end = now() + 10000000000ULL;
    int entered = 0;
    while (now() < end) {
        if (read(notices[0], &event, sizeof(event)) == sizeof(event)) {
            entered = 1;
            break;
        }
        CHECK(errno == EAGAIN || errno == EINTR);
        pause_short();
    }
    CHECK(entered && event.caller == client && event.offset == 4096);
    unsigned blocked = 0;
    end = now() + 10000000000ULL;
    while (now() < end && (blocked = reply_blocked(client)) < CLIENTS)
        pause_short();
    CHECK(blocked == CLIENTS);
    puts("PASS saturation precondition: all 32 admitted read clients are kernel REPLY-blocked "
         "while one callback is held");
    int completed = 0;
    end = now() + 17000000000ULL;
    while (now() < end) {
        completed = 0;
        for (int i = 0; i < CLIENTS; i++)
            completed += atomic_load(&shared->result[i].done) != 0;
        if (completed == CLIENTS)
            break;
        pause_short();
    }
    int valid = completed == CLIENTS;
    for (int i = 0; i < CLIENTS && valid; i++) {
        struct result *r = &shared->result[i];
        valid = r->returned == -1 && r->error == ETIMEDOUT && r->finished >= r->deadline &&
                r->finished - r->deadline < 1000000000ULL;
    }
    printf("%s all 32 kernel reply waits return ETIMEDOUT before callback release, within the "
           "functional deadline\n",
           valid ? "PASS" : "FAIL");
    CHECK(write(releases[1], "x", 1) == 1);
    CHECK(valid);
    int status;
    CHECK(waitpid(client, &status, 0) == client && WIFEXITED(status) && !WEXITSTATUS(status));
    client = 0;
    close(file);
    cleanup();
    alarm(0);
    return 0;
}
