/* SPDX-License-Identifier: MIT
 * Native management-plane identity, authorization and stopped-process checks.
 */
#include <qfuse/admin.h>
#include <sys/neutrino.h>
#include <sys/netmgr.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static pid_t server;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL admin line %d: %s errno=%d\n", __LINE__, #x, errno);             \
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
    struct timespec t = {0, 10000000};
    nanosleep(&t, NULL);
}
static void cleanup(void)
{
    if (server > 0) {
        kill(server, SIGCONT);
        kill(server, SIGKILL);
        waitpid(server, NULL, 0);
        server = 0;
    }
}
static void timeout_test(int n)
{
    (void)n;
    if (server > 0)
        kill(server, SIGKILL);
    _exit(124);
}
static int call(const struct qfuse_admin_advert *a, unsigned command, int wrong,
                struct qfuse_admin_status *s)
{
    int coid = ConnectAttach(ND_LOCAL_NODE, a->pid, a->channel, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1)
        return -1;
    struct _server_info info;
    CHECK(ConnectServerInfo(0, coid, &info) != -1 && info.pid == (pid_t)a->pid);
    struct qfuse_admin_request r;
    memset(&r, 0, sizeof(r));
    r.magic = QFUSE_ADMIN_MAGIC;
    r.version = QFUSE_ADMIN_VERSION;
    r.command = command;
    memcpy(r.instance, a->instance, sizeof(r.instance));
    if (wrong)
        r.instance[0] ^= 1;
    uint64_t limit = 500000000ULL;
    CHECK(
        !TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &limit, NULL));
    memset(s, 0, sizeof(*s));
    int n = MsgSend(coid, &r, sizeof(r), s, sizeof(*s));
    int e = errno;
    ConnectDetach(coid);
    errno = e;
    return n;
}
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    atexit(cleanup);
    signal(SIGALRM, timeout_test);
    alarm(30);
    int p[2];
    CHECK(!pipe(p));
    CHECK(fcntl(p[0], F_SETFL, O_NONBLOCK) != -1);
    server = fork();
    CHECK(server >= 0);
    if (!server) {
        close(p[0]);
        char n[32];
        snprintf(n, sizeof(n), "%d", p[1]);
        setenv("QFUSE_ADMIN_FD", n, 1);
        execl("/data/qfuse/qfuse-hello", "hello", "-f", "/qfuse-admin-test", NULL);
        _exit(127);
    }
    close(p[1]);
    struct qfuse_admin_advert a;
    size_t have = 0;
    uint64_t end = now() + 10000000000ULL;
    while (have < sizeof(a) && now() < end) {
        ssize_t n = read(p[0], (char *)&a + have, sizeof(a) - have);
        if (n > 0)
            have += (size_t)n;
        else
            CHECK(n < 0 && (errno == EAGAIN || errno == EINTR));
        pause_short();
    }
    close(p[0]);
    CHECK(have == sizeof(a) && a.magic == QFUSE_ADMIN_MAGIC && a.pid == (uint32_t)server);
    struct qfuse_admin_status s;
    end = now() + 5000000000ULL;
    int ready = 0;
    while (now() < end) {
        if (!call(&a, QFUSE_ADMIN_STATUS, 0, &s) && s.phase == QFUSE_ADMIN_SERVING) {
            ready = 1;
            break;
        }
        pause_short();
    }
    CHECK(ready && s.pid == (uint32_t)server && !strcmp(s.mountpoint, "/qfuse-admin-test"));
    puts("PASS admin status: kernel peer identity and independent management channel");
    errno = 0;
    CHECK(call(&a, QFUSE_ADMIN_STOP, 1, &s) == -1 && errno == ESTALE);
    CHECK(!call(&a, QFUSE_ADMIN_STATUS, 0, &s) && s.phase == QFUSE_ADMIN_SERVING);
    puts("PASS admin stale-instance guard: wrong nonce cannot stop a filesystem");
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        if (setgid(65534) || setuid(65534))
            _exit(3);
        errno = 0;
        int n = call(&a, QFUSE_ADMIN_STATUS, 0, &s);
        _exit(n == -1 && errno == EPERM ? 0 : 4);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    puts("PASS admin authorization: non-root client rejected by kernel credentials");
    CHECK(!kill(server, SIGSTOP));
    CHECK(waitpid(server, &status, WUNTRACED) == server && WIFSTOPPED(status));
    uint64_t started = now();
    errno = 0;
    CHECK(call(&a, QFUSE_ADMIN_STATUS, 0, &s) == -1 && errno == ETIMEDOUT &&
          now() - started < 1500000000ULL);
    CHECK(!kill(server, SIGCONT));
    CHECK(!call(&a, QFUSE_ADMIN_STATUS, 0, &s) && s.phase == QFUSE_ADMIN_SERVING);
    puts("PASS admin control timeout: stopped process cannot trap a management caller; later query "
         "succeeds");
    CHECK(!call(&a, QFUSE_ADMIN_STOP, 0, &s));
    CHECK(waitpid(server, &status, 0) == server && WIFEXITED(status) && !WEXITSTATUS(status));
    server = 0;
    alarm(0);
    puts("PASS admin graceful stop: idle mount exits and removes its channels");
    return 0;
}
