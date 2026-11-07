/* SPDX-License-Identifier: MIT — native cross-path isolation and rename lifetime. */
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include "inflight_protocol.h"
struct outcome {
    _Atomic int done;
    int result, error;
};
static struct shared {
    struct outcome held, work, reader, closer;
    _Atomic int closer_ready, close_go;
} *shared;
static pid_t server, holder, worker, reader, closer;
static char root[256], view[512], held_path[512], held_view[768];
static int notices[2], releases[2], stable_fd = -1;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL namespace line %d: %s (%s)\n", __LINE__, #x, strerror(errno));   \
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
    struct timespec p = {0, 1000000};
    nanosleep(&p, NULL);
}
static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        remove_tree(p);
    }
    closedir(d);
    rmdir(path);
}
static void cleanup(void)
{
    if (server > 0) {
        kill(server, SIGKILL);
        waitpid(server, NULL, 0);
        server = 0;
    }
    pid_t *children[] = {&holder, &worker, &reader, &closer};
    for (unsigned i = 0; i < 4; i++)
        if (*children[i] > 0) {
            kill(*children[i], SIGKILL);
            waitpid(*children[i], NULL, 0);
            *children[i] = 0;
        }
    if (stable_fd >= 0) {
        close(stable_fd);
        stable_fd = -1;
    }
    if (root[0])
        remove_tree(root);
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
    if (holder > 0)
        kill(holder, SIGKILL);
    if (worker > 0)
        kill(worker, SIGKILL);
    if (reader > 0)
        kill(reader, SIGKILL);
    if (closer > 0)
        kill(closer, SIGKILL);
    _exit(124);
}
static void publish(struct outcome *o, int result)
{
    o->result = result;
    o->error = errno;
    atomic_store(&o->done, 1);
}
static int await(struct outcome *o, unsigned seconds)
{
    uint64_t end = now() + seconds * 1000000000ULL;
    while (!atomic_load(&o->done) && now() < end)
        pause_short();
    return atomic_load(&o->done);
}
static void reap(pid_t *p)
{
    int status;
    CHECK(waitpid(*p, &status, 0) == *p && WIFEXITED(status) && !WEXITSTATUS(status));
    *p = 0;
}
static int workflow(void)
{
    char dir[768], file[768], renamed[768];
    snprintf(dir, sizeof(dir), "%s/independent", view);
    snprintf(file, sizeof(file), "%s/newfile", view);
    snprintf(renamed, sizeof(renamed), "%s/renamed", view);
    if (mkdir(dir, 0700))
        return -1;
    int f = open(file, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (f < 0)
        return -1;
    if (write(f, "updated", 7) != 7 || close(f))
        return -1;
    if (rename(file, renamed))
        return -1;
    f = open(renamed, O_RDONLY);
    if (f < 0)
        return -1;
    char bytes[8] = {0};
    if (read(f, bytes, sizeof(bytes)) != 7 || memcmp(bytes, "updated", 7) || close(f))
        return -1;
    if (unlink(renamed) || rmdir(dir))
        return -1;
    return 0;
}
static unsigned thread_state_count(pid_t pid, const char *state)
{
    int pipefd[2];
    CHECK(!pipe(pipefd));
    pid_t probe = fork();
    CHECK(probe >= 0);
    if (!probe) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        char number[32];
        snprintf(number, sizeof(number), "%d", pid);
        execlp("pidin", "pidin", "-p", number, NULL);
        _exit(127);
    }
    close(pipefd[1]);
    FILE *stream = fdopen(pipefd[0], "r");
    CHECK(stream);
    char line[512];
    unsigned count = 0;
    while (fgets(line, sizeof(line), stream))
        if (strstr(line, state))
            count++;
    fclose(stream);
    int status;
    CHECK(waitpid(probe, &status, 0) == probe && WIFEXITED(status) && !WEXITSTATUS(status));
    return count;
}
int main(int argc, char **argv)
{
    const char *operation = argc == 2 ? argv[1] : "read";
    int grace_mode = !strcmp(operation, "grace");
    int serial_close = !strcmp(operation, "serial-close");
    int flush_error = !strcmp(operation, "flush-error");
    if (grace_mode || serial_close)
        operation = "rename";
    if (flush_error)
        operation = "read";
    int rename_mode = !strcmp(operation, "rename"), mkdir_mode = !strcmp(operation, "mkdir");
    int fair_mode = !strcmp(operation, "fair");
    CHECK(argc <= 2 && (rename_mode || mkdir_mode || fair_mode || !strcmp(operation, "read")));
    setvbuf(stdout, NULL, _IOLBF, 0);
    atexit(cleanup);
    signal(SIGALRM, timeout_test);
    alarm(60);
    shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
    CHECK(shared != MAP_FAILED);
    memset(shared, 0, sizeof(*shared));
    strcpy(root, "/data/qfuse-namespace.XXXXXX");
    CHECK(mkdtemp(root));
    snprintf(view, sizeof(view), "/qfuse-namespace%s", root);
    snprintf(held_path, sizeof(held_path), "%s/%s", root,
             mkdir_mode    ? "held-dir"
             : rename_mode ? "tree"
                           : "big");
    snprintf(held_view, sizeof(held_view), "/qfuse-namespace%s", held_path);
    char source[768];
    snprintf(source, sizeof(source), "%s%s", held_path, rename_mode ? "/payload" : "");
    if (rename_mode)
        CHECK(!mkdir(held_path, 0700));
    if (!mkdir_mode) {
        int f = open(source, O_CREAT | O_EXCL | O_WRONLY, 0600);
        CHECK(f >= 0);
        char bytes[8192];
        memset(bytes, 'Q', sizeof(bytes));
        CHECK(write(f, bytes, sizeof(bytes)) == sizeof(bytes) && !close(f));
    }
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
        setenv("QFUSE_INFLIGHT_PATH", held_path, 1);
        setenv("QFUSE_INFLIGHT_OPERATION", operation, 1);
        if (flush_error)
            setenv("QFUSE_FLUSH_ERROR", "1", 1);
        if (serial_close)
            setenv("QFUSE_ENABLE_FLUSH", "1", 1);
        char release_path[768];
        snprintf(release_path, sizeof(release_path), "%s/release-check", root);
        setenv("QFUSE_RELEASE_NOTICE_PATH", release_path, 1);
        const char *binary = getenv("QFUSE_INFLIGHT_BINARY");
        if (serial_close || flush_error)
            execl(binary ? binary : "/data/qfuse/qfuse-inflight", "inflight", "-f", "-s",
                  "/qfuse-namespace", NULL);
        execl(binary ? binary : "/data/qfuse/qfuse-inflight", "inflight", "-f", "/qfuse-namespace",
              NULL);
        _exit(127);
    }
    uint64_t ready_end = now() + 10000000000ULL;
    for (;;) {
        struct stat st;
        if (!stat(view, &st))
            break;
        CHECK(now() < ready_end);
        pause_short();
    }
    if (flush_error) {
        int f = open(held_view, O_WRONLY);
        CHECK(f >= 0 && write(f, "W", 1) == 1);
        errno = 0;
        int result = close(f), error = errno;
        CHECK(result == -1 && error == EIO);
        CHECK(fcntl(f, F_GETFD) == -1 && errno == EBADF);
        kill(server, SIGTERM);
        int status;
        CHECK(waitpid(server, &status, 0) == server && WIFEXITED(status) && !WEXITSTATUS(status));
        server = 0;
        cleanup();
        root[0] = 0;
        alarm(0);
        puts("PASS synchronous close preserves filesystem flush errors and closes the descriptor");
        return 0;
    }
    if (rename_mode || fair_mode) {
        char p[1024];
        snprintf(p, sizeof(p), "%s%s", held_view, rename_mode ? "/payload" : "");
        stable_fd = open(p, O_RDONLY);
        CHECK(stable_fd >= 0);
    }
    if (rename_mode) {
        closer = fork();
        CHECK(closer >= 0);
        if (!closer) {
            char p[1024];
            snprintf(p, sizeof(p), "%s/payload", held_view);
            int f = open(p, O_RDONLY);
            if (f < 0)
                _exit(5);
            atomic_store(&shared->closer_ready, 1);
            while (!atomic_load(&shared->close_go))
                pause_short();
            int result = close(f), saved = errno;
            int accepted = result == 0 || (!serial_close && result == -1 && saved == EAGAIN);
            errno = saved;
            publish(&shared->closer, accepted ? 0 : -1);
            _exit(accepted ? 0 : 1);
        }
        uint64_t close_end = now() + 5000000000ULL;
        while (!atomic_load(&shared->closer_ready)) {
            CHECK(now() < close_end);
            pause_short();
        }
    }
    holder = fork();
    CHECK(holder >= 0);
    if (!holder) {
        int result;
        if (mkdir_mode)
            result = mkdir(held_view, 0700);
        else if (rename_mode) {
            char moved[768];
            snprintf(moved, sizeof(moved), "%s/moved", view);
            result = rename(held_view, moved);
        } else {
            int f = open(held_view, O_RDONLY);
            if (f < 0)
                _exit(2);
            char bytes[4096];
            result = pread(f, bytes, sizeof(bytes), 4096) == sizeof(bytes) ? 0 : -1;
            close(f);
        }
        publish(&shared->held, result);
        _exit(result ? 1 : 0);
    }
    struct qf_inflight_event event;
    uint64_t end = now() + 10000000000ULL;
    int seen = 0;
    while (now() < end) {
        if (read(notices[0], &event, sizeof(event)) == sizeof(event)) {
            seen = 1;
            break;
        }
        CHECK(errno == EAGAIN || errno == EINTR);
        pause_short();
    }
    CHECK(seen && event.caller == holder);
    printf("PASS namespace precondition: %s callback is held\n", operation);
    if (serial_close) {
        unsigned before = thread_state_count(server, "CONDVAR");
        atomic_store(&shared->close_go, 1);
        uint64_t end = now() + 5000000000ULL;
        while (thread_state_count(server, "CONDVAR") <= before) {
            CHECK(!atomic_load(&shared->closer.done) && now() < end);
            pause_short();
        }
        CHECK(!atomic_load(&shared->closer.done));
        CHECK(write(releases[1], "x", 1) == 1);
        CHECK(await(&shared->held, 5) && !shared->held.result);
        reap(&holder);
        CHECK(await(&shared->closer, 5) && !shared->closer.result);
        reap(&closer);
        kill(server, SIGTERM);
        int status;
        CHECK(waitpid(server, &status, 0) == server && WIFEXITED(status) && !WEXITSTATUS(status));
        server = 0;
        cleanup();
        root[0] = 0;
        alarm(0);
        puts("PASS ordinary single-thread close queues safely and reports successful flush");
        return 0;
    }
    if (rename_mode) {
        atomic_store(&shared->close_go, 1);
        CHECK(await(&shared->closer, 3) && !shared->closer.result);
        reap(&closer);
        puts("PASS cleanup precondition: independently opened handle closes while rename stays "
             "held");
    }
    for (int priority = 0; priority <= 50; priority += 50) {
        atomic_store(&shared->work.done, 0);
        worker = fork();
        CHECK(worker >= 0);
        if (!worker) {
            if (priority) {
                struct sched_param p = {.sched_priority = priority};
                if (sched_setscheduler(0, SCHED_RR, &p))
                    _exit(3);
            }
            int result = workflow();
            publish(&shared->work, result);
            _exit(result ? 1 : 0);
        }
        CHECK(await(&shared->work, 3) && shared->work.result == 0);
        reap(&worker);
        printf("PASS independent mkdir/create/write/rename/read/unlink at priority %d while %s "
               "stays held\n",
               priority, operation);
    }
    if (rename_mode) {
        char release_view[768];
        snprintf(release_view, sizeof(release_view), "%s/release-check", view);
        int f = open(release_view, O_CREAT | O_EXCL | O_WRONLY, 0600);
        CHECK(f >= 0 && !close(f));
        uint64_t cleanup_end = now() + 3000000000ULL;
        int released = 0;
        while (now() < cleanup_end) {
            if (read(notices[0], &event, sizeof(event)) == sizeof(event)) {
                CHECK(event.offset == -3);
                released = 1;
                break;
            }
            CHECK(errno == EAGAIN || errno == EINTR);
            pause_short();
        }
        CHECK(released);
        puts("PASS unrelated release callback completes while parent rename stays held");
    }
    CHECK(!atomic_load(&shared->held.done));
    if (rename_mode) {
        for (int pass = 0; pass < 2; pass++) {
            atomic_store(&shared->reader.done, 0);
            unsigned before = thread_state_count(server, "CONDVAR");
            reader = fork();
            CHECK(reader >= 0);
            if (!reader) {
                struct sigaction action = {0};
                action.sa_handler = signal_client;
                sigemptyset(&action.sa_mask);
                if (sigaction(SIGUSR1, &action, NULL))
                    _exit(4);
                char byte;
                errno = 0;
                int n = pread(stable_fd, &byte, 1, 0), error = errno;
                int result = pass ? (n == 1 && byte == 'Q' ? 0 : -1) : n;
                errno = error;
                publish(&shared->reader, result);
                _exit(pass ? (result ? 1 : 0) : (n == -1 && error == EINTR ? 0 : 1));
            }
            uint64_t queued_end = now() + 5000000000ULL;
            while (thread_state_count(server, "CONDVAR") <= before) {
                CHECK(now() < queued_end);
                pause_short();
            }
            CHECK(thread_state_count(reader, "REPLY") && !atomic_load(&shared->reader.done));
            puts("PASS rename precondition: descriptor reader is queued on a namespace condition");
            if (!pass) {
                CHECK(!kill(reader, SIGUSR1));
                CHECK(await(&shared->reader, 1) && shared->reader.result == -1 &&
                      shared->reader.error == EINTR);
                reap(&reader);
                queued_end = now() + 5000000000ULL;
                while (thread_state_count(server, "CONDVAR") > before) {
                    CHECK(now() < queued_end);
                    pause_short();
                }
                puts("PASS namespace condition wait cancels and its worker unwinds before rename "
                     "returns");
            }
        }
    }
    if (fair_mode) {
        unsigned before = thread_state_count(server, "CONDVAR");
        atomic_store(&shared->work.done, 0);
        worker = fork();
        CHECK(worker >= 0);
        if (!worker) {
            int f = open(held_view, O_WRONLY | O_TRUNC);
            int result = f < 0 ? -1 : 0;
            if (f >= 0)
                close(f);
            publish(&shared->work, result);
            _exit(result ? 1 : 0);
        }
        uint64_t queued_end = now() + 5000000000ULL;
        while (thread_state_count(server, "CONDVAR") <= before) {
            CHECK(now() < queued_end);
            pause_short();
        }
        CHECK(thread_state_count(worker, "REPLY") && !atomic_load(&shared->work.done));
        before = thread_state_count(server, "CONDVAR");
        reader = fork();
        CHECK(reader >= 0);
        if (!reader) {
            char byte;
            int result = pread(stable_fd, &byte, 1, 0) == 0 ? 0 : -1;
            publish(&shared->reader, result);
            _exit(result ? 1 : 0);
        }
        queued_end = now() + 5000000000ULL;
        while (thread_state_count(server, "CONDVAR") <= before) {
            CHECK(now() < queued_end);
            pause_short();
        }
        CHECK(thread_state_count(reader, "REPLY") && !atomic_load(&shared->reader.done));
        puts("PASS fairness precondition: writer and later reader are queued behind held read");
    }
    if (grace_mode) {
        CHECK(!kill(server, SIGTERM));
        uint64_t grace_end = now() + 250000000ULL;
        do {
            int status;
            pid_t observed = waitpid(server, &status, WNOHANG);
            if (observed == server)
                server = 0;
            CHECK(!observed && !atomic_load(&shared->held.done) &&
                  !atomic_load(&shared->reader.done));
            pause_short();
        } while (now() < grace_end);
        puts("PASS graceful stop retains the active callback and queued reader until release");
    }
    CHECK(write(releases[1], "x", 1) == 1);
    CHECK(await(&shared->held, 5) && !shared->held.result);
    reap(&holder);
    if (rename_mode) {
        CHECK(await(&shared->reader, 5) && !shared->reader.result);
        reap(&reader);
        puts("PASS queued descriptor read survives parent rename");
    }
    if (fair_mode) {
        CHECK(await(&shared->work, 5) && !shared->work.result);
        reap(&worker);
        CHECK(await(&shared->reader, 5) && !shared->reader.result);
        reap(&reader);
        puts("PASS queued truncate writer runs before a later reader observes EOF");
    }
    if (!grace_mode)
        kill(server, SIGTERM);
    int status;
    CHECK(waitpid(server, &status, 0) == server && WIFEXITED(status) && !WEXITSTATUS(status));
    server = 0;
    cleanup();
    root[0] = 0;
    alarm(0);
    puts("PASS scoped namespace isolation and cleanup");
    return 0;
}
