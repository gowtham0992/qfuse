/* SPDX-License-Identifier: MIT
 * Functional deadline gate on the QNX monotonic clock, not a benchmark.
 */
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
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

static pid_t server, copier;
static int notify_pipe[2] = {-1, -1}, release_pipe[2] = {-1, -1};
static char fixture[256], source_path[512], partial_path[512], view_path[768];
struct result {
    _Atomic int done;
    ssize_t returned;
    int error;
    uint64_t finished;
};
static struct result *result;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL in-flight line %d: %s errno=%d (%s)\n", __LINE__, #x, errno,     \
                    strerror(errno));                                                              \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static uint64_t now(void)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
static void cleanup(void)
{
    if (server > 0) {
        kill(server, SIGKILL);
        waitpid(server, NULL, 0);
        server = 0;
    }
    if (copier > 0) {
        kill(copier, SIGKILL);
        waitpid(copier, NULL, 0);
        copier = 0;
    }
    for (int i = 0; i < 2; i++) {
        if (notify_pipe[i] >= 0)
            close(notify_pipe[i]);
        if (release_pipe[i] >= 0)
            close(release_pipe[i]);
    }
    if (partial_path[0])
        unlink(partial_path);
    if (source_path[0])
        unlink(source_path);
    if (fixture[0])
        rmdir(fixture);
}
static void timeout(int sig)
{
    (void)sig;
    if (server > 0)
        kill(server, SIGKILL);
    if (copier > 0)
        kill(copier, SIGKILL);
    _exit(124);
}
static void start(int intercept)
{
    server = fork();
    CHECK(server >= 0);
    if (!server) {
        close(notify_pipe[0]);
        close(release_pipe[1]);
        if (intercept) {
            char a[32], b[32];
            snprintf(a, sizeof(a), "%d", notify_pipe[1]);
            snprintf(b, sizeof(b), "%d", release_pipe[0]);
            setenv("QFUSE_INFLIGHT_NOTIFY_FD", a, 1);
            setenv("QFUSE_INFLIGHT_RELEASE_FD", b, 1);
            setenv("QFUSE_INFLIGHT_PATH", source_path, 1);
            execl("/data/qfuse/qfuse-inflight", "inflight-passthrough", "-f", "-o", "ro",
                  "/qfuse-inflight", NULL);
        } else {
            const char *binary = getenv("QFUSE_PASSTHROUGH_BINARY");
            execl(binary ? binary : "/data/qfuse/qfuse-passthrough", "passthrough", "-f", "-o",
                  "ro", "/qfuse-inflight", NULL);
        }
        _exit(127);
    }
    for (int i = 0; i < 200; i++) {
        struct stat st;
        if (!stat(view_path, &st))
            return;
        int status;
        if (waitpid(server, &status, WNOHANG) == server) {
            server = 0;
            CHECK(!"server exited before readiness");
        }
        struct timespec pause = {0, 100000000};
        nanosleep(&pause, NULL);
    }
    CHECK(!"mount readiness deadline");
}
static int reply_blocked(pid_t expected_client, pid_t expected_server)
{
    int output[2];
    CHECK(pipe(output) == 0);
    pid_t inspection = fork();
    CHECK(inspection >= 0);
    if (!inspection) {
        dup2(output[1], STDOUT_FILENO);
        close(output[0]);
        close(output[1]);
        char pid[32];
        snprintf(pid, sizeof(pid), "%ld", (long)expected_client);
        execlp("pidin", "pidin", "-p", pid, "-F", "%a %b %J %B", NULL);
        _exit(127);
    }
    close(output[1]);
    char text[4096];
    size_t used = 0;
    ssize_t n;
    while ((n = read(output[0], text + used, sizeof(text) - 1 - used)) > 0) {
        used += n;
        CHECK(used < sizeof(text) - 1);
    }
    CHECK(n == 0);
    text[used] = 0;
    close(output[0]);
    int status;
    CHECK(waitpid(inspection, &status, 0) == inspection && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    fputs(text, stdout);
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        long pid, blocked;
        unsigned tid;
        char state[32];
        if (sscanf(line, "%ld %u %31s %ld", &pid, &tid, state, &blocked) == 4 &&
            pid == expected_client && tid == 1 && !strcmp(state, "REPLY") &&
            blocked == expected_server)
            return 1;
    }
    return 0;
}
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGALRM, timeout);
    alarm(60);
    atexit(cleanup);
    strcpy(fixture, "/data/qfuse-inflight.XXXXXX");
    CHECK(mkdtemp(fixture));
    snprintf(source_path, sizeof(source_path), "%s/input", fixture);
    snprintf(partial_path, sizeof(partial_path), "%s/partial", fixture);
    snprintf(view_path, sizeof(view_path), "/qfuse-inflight%s", source_path);
    unsigned char bytes[QF_INFLIGHT_CHUNK];
    for (size_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (unsigned char)((i * 37) ^ (i >> 8));
    int fd = open(source_path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    CHECK(write(fd, bytes, sizeof(bytes)) == sizeof(bytes) &&
          write(fd, bytes, sizeof(bytes)) == sizeof(bytes) && close(fd) == 0);
    CHECK(pipe(notify_pipe) == 0 && pipe(release_pipe) == 0);
    result = mmap(NULL, sizeof(*result), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    CHECK(result != MAP_FAILED);
    memset(result, 0, sizeof(*result));
    atomic_init(&result->done, 0);
    CHECK(atomic_is_lock_free(&result->done));
    start(1);
    copier = fork();
    CHECK(copier >= 0);
    if (!copier) {
        close(notify_pipe[0]);
        close(notify_pipe[1]);
        close(release_pipe[0]);
        close(release_pipe[1]);
        int input = open(view_path, O_RDONLY | O_CLOEXEC),
            output = open(partial_path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        CHECK(input >= 0 && output >= 0);
        unsigned char data[QF_INFLIGHT_CHUNK];
        CHECK(read(input, data, sizeof(data)) == sizeof(data) &&
              !memcmp(data, bytes, sizeof(data)) &&
              write(output, data, sizeof(data)) == sizeof(data));
        errno = 0;
        ssize_t returned = read(input, data, sizeof(data));
        int saved = errno;
        uint64_t finished = now();
        result->returned = returned;
        result->error = saved;
        result->finished = finished;
        atomic_store_explicit(&result->done, 1, memory_order_release);
        close(input);
        CHECK(close(output) == 0);
        _exit(0);
    }
    close(notify_pipe[1]);
    notify_pipe[1] = -1;
    close(release_pipe[0]);
    release_pipe[0] = -1;
    struct qf_inflight_event entered;
    CHECK(read(notify_pipe[0], &entered, sizeof(entered)) == sizeof(entered));
    CHECK(entered.caller == copier && entered.offset == QF_INFLIGHT_CHUNK);
    CHECK(!atomic_load_explicit(&result->done, memory_order_acquire));
    CHECK(reply_blocked(copier, server));
    puts("PASS in-flight precondition: real copier is kernel REPLY-blocked inside the held "
         "passthrough read");
    uint64_t killed_at = now();
    if (!getenv("QFUSE_INFLIGHT_LEAVE_SERVER_ALIVE"))
        CHECK(kill(server, SIGKILL) == 0);
    while (!atomic_load_explicit(&result->done, memory_order_acquire)) {
        if (now() - killed_at >= 1000000000ULL) {
            fputs("FAIL in-flight deadline: no client result before the required deadline\n",
                  stderr);
            exit(1);
        }
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    CHECK(result->returned == -1 && result->error != 0 && result->error != ETIMEDOUT &&
          result->error != EINTR);
    CHECK(result->finished >= killed_at && result->finished - killed_at < 1000000000ULL);
    int status;
    CHECK(waitpid(server, &status, 0) == server && WIFSIGNALED(status) &&
          WTERMSIG(status) == SIGKILL);
    server = 0;
    CHECK(waitpid(copier, &status, 0) == copier && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    copier = 0;
    struct stat partial;
    CHECK(stat(partial_path, &partial) == 0 && partial.st_size == QF_INFLIGHT_CHUNK);
    puts("PASS strict death deadline: error returned from the already-blocked read before the "
         "required deadline; partial copy rejected");
    start(0);
    fd = open(view_path, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    unsigned char readback[QF_INFLIGHT_CHUNK];
    CHECK(read(fd, readback, sizeof(readback)) == sizeof(readback) &&
          !memcmp(readback, bytes, sizeof(bytes)));
    CHECK(read(fd, readback, sizeof(readback)) == sizeof(readback) &&
          !memcmp(readback, bytes, sizeof(bytes)));
    CHECK(read(fd, readback, 1) == 0 && close(fd) == 0);
    CHECK(kill(server, SIGTERM) == 0 && waitpid(server, &status, 0) == server &&
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
    server = 0;
    CHECK(munmap(result, sizeof(*result)) == 0);
    alarm(0);
    puts("PASS post-crash remount: normal upstream passthrough returned the complete unchanged "
         "file");
    return 0;
}
