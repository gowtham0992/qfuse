/* SPDX-License-Identifier: MIT
 * Root-only background launcher. Control uses a separate native channel, never
 * opens the mounted tree, and verifies both kernel peer PID and instance nonce.
 */
#include <qfuse/admin.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/netmgr.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#define PATH_SIZE 4097
#define RECORD_MAGIC UINT32_C(0x51464d31)
struct mount_record {
    uint32_t magic, version, pid, channel;
    unsigned char instance[16];
    char mount[PATH_SIZE];
};
static pid_t starting_child;
static char starting_record[PATH_SIZE];
static volatile sig_atomic_t interrupted;
static mode_t child_umask;
static void note_signal(int n)
{
    interrupted = n;
}
static void fail(const char *message)
{
    fprintf(stderr, "qfuse-mount: %s: %s\n", message, strerror(errno));
    exit(1);
}
static uint64_t now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        fail("clock");
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
static void pause_short(void)
{
    struct timespec t = {0, 50000000};
    nanosleep(&t, NULL);
}
static int clean_absolute(const char *p)
{
    if (!p || p[0] != '/' || !p[1] || strlen(p) >= PATH_SIZE)
        return 0;
    for (const unsigned char *c = (const unsigned char *)p; *c; c++)
        if (*c < 32 || *c == 127)
            return 0;
    const char *s = p + 1;
    for (;;) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (!n || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
            return 0;
        if (!e)
            return 1;
        s = e + 1;
    }
}
static int beneath(const char *p, const char *root)
{
    size_t n = strlen(root);
    return !strncmp(p, root, n) && (!p[n] || p[n] == '/');
}
static void private_directory(const char *path)
{
    char p[PATH_SIZE];
    strcpy(p, path);
    for (char *c = p + 1;; c++) {
        if (*c && *c != '/')
            continue;
        char saved = *c;
        *c = 0;
        struct stat st;
        if (lstat(p, &st)) {
            if (errno != ENOENT || mkdir(p, 0700) || lstat(p, &st))
                fail("create state directory");
        }
        if (!S_ISDIR(st.st_mode) || st.st_uid != 0 ||
            ((st.st_mode & 0022) && !(st.st_mode & S_ISVTX))) {
            errno = EPERM;
            fail("state directory ancestors must be trusted root-owned directories");
        }
        *c = saved;
        if (!saved)
            break;
    }
    struct stat st;
    if (lstat(path, &st) || st.st_mode & 0077) {
        errno = EPERM;
        fail("state directory must have mode 0700");
    }
}
static void paths(const char *mount, char record[PATH_SIZE], char lock[PATH_SIZE],
                  char log[PATH_SIZE])
{
    const char *dir = getenv("QFUSE_STATE_DIR");
    if (!dir)
        dir = "/data/qfuse/run";
    if (!clean_absolute(dir) || beneath(dir, mount)) {
        errno = EINVAL;
        fail("state storage must be outside the mount");
    }
    private_directory(dir);
    uint64_t h = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)mount; *p; p++)
        h = (h ^ *p) * UINT64_C(1099511628211);
    int a = snprintf(record, PATH_SIZE, "%s/%016" PRIx64 ".state", dir, h),
        b = snprintf(lock, PATH_SIZE, "%s/%016" PRIx64 ".lock", dir, h),
        c = snprintf(log, PATH_SIZE, "%s/%016" PRIx64 ".log", dir, h);
    if (a >= PATH_SIZE || b >= PATH_SIZE || c >= PATH_SIZE) {
        errno = ENAMETOOLONG;
        fail("state path");
    }
}
static int secure_open(const char *path, int flags)
{
    int fd = open(path, (flags & ~O_TRUNC) | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd == -1)
        return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077) ||
        st.st_nlink != 1) {
        int e = EPERM;
        close(fd);
        errno = e;
        return -1;
    }
    if ((flags & O_TRUNC) && ftruncate(fd, 0)) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}
static int lock_record(const char *path)
{
    int fd = secure_open(path, O_RDWR | O_CREAT);
    if (fd == -1)
        fail("open state lock");
    struct flock lock = {0};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &lock) == -1) {
        close(fd);
        fail("another mount action is in progress");
    }
    return fd;
}
static int read_record(const char *path, const char *mount, struct mount_record *r)
{
    int fd = secure_open(path, O_RDONLY);
    if (fd == -1)
        return -1;
    ssize_t n = read(fd, r, sizeof(*r));
    close(fd);
    if (n != sizeof(*r) || r->magic != RECORD_MAGIC || r->version != 1 || !r->pid ||
        !memchr(r->mount, 0, sizeof(r->mount)) || strcmp(r->mount, mount)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}
static int control(const struct mount_record *r, unsigned command, unsigned flags,
                   struct qfuse_admin_status *status, int verify_nonce)
{
    int coid = ConnectAttach(ND_LOCAL_NODE, r->pid, r->channel, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1)
        return -1;
    struct _server_info peer;
    if (ConnectServerInfo(0, coid, &peer) == -1 || peer.pid != (pid_t)r->pid ||
        peer.chid != (int)r->channel) {
        ConnectDetach(coid);
        errno = ESTALE;
        return -1;
    }
    struct qfuse_admin_request request;
    memset(&request, 0, sizeof(request));
    request.magic = QFUSE_ADMIN_MAGIC;
    request.version = QFUSE_ADMIN_VERSION;
    request.command = command;
    request.flags = flags;
    memcpy(request.instance, r->instance, sizeof(request.instance));
    memset(status, 0, sizeof(*status));
    uint64_t timeout = 1000000000ULL;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout,
                     NULL) == -1) {
        ConnectDetach(coid);
        return -1;
    }
    int result = MsgSend(coid, &request, sizeof(request), status, sizeof(*status));
    int e = errno;
    ConnectDetach(coid);
    if (result == -1) {
        errno = e;
        return -1;
    }
    if (status->magic != QFUSE_ADMIN_MAGIC || status->version != QFUSE_ADMIN_VERSION ||
        status->size != sizeof(*status) || status->pid != r->pid || status->channel != r->channel ||
        !memchr(status->mountpoint, 0, sizeof(status->mountpoint)) ||
        strcmp(status->mountpoint, r->mount) ||
        (verify_nonce && memcmp(status->instance, r->instance, sizeof(r->instance)))) {
        errno = ESTALE;
        return -1;
    }
    return 0;
}
static const char *phase_name(unsigned phase)
{
    switch (phase) {
    case QFUSE_ADMIN_STAGING:
        return "starting";
    case QFUSE_ADMIN_SERVING:
        return "ready";
    case QFUSE_ADMIN_STOPPING:
        return "stopping";
    default:
        return "failed";
    }
}
static void print_status(const struct qfuse_admin_status *s)
{
    printf("%s: %s (pid %u)\n", s->mountpoint, phase_name(s->phase), s->pid);
    printf("Operations: %u/%u; cancelled waits: %u; deferred cleanup: %u\n", s->active_operations,
           s->operation_limit, s->cancelled_waits, s->cleanup_pending);
}
static void cleanup_start(void)
{
    if (starting_child > 0) {
        pid_t group = getpgid(starting_child);
        if (group == starting_child)
            kill(-group, SIGKILL);
        else
            kill(starting_child, SIGKILL);
        while (waitpid(starting_child, NULL, 0) == -1 && errno == EINTR) {
        }
        starting_child = 0;
    }
    if (starting_record[0])
        unlink(starting_record);
}
static int all_zero(const unsigned char *p, size_t n)
{
    unsigned value = 0;
    while (n--)
        value |= *p++;
    return !value;
}
static void save_record(int fd, const struct mount_record *r)
{
    if (pwrite(fd, r, sizeof(*r), 0) != sizeof(*r) || ftruncate(fd, sizeof(*r)) || fsync(fd))
        fail("write mount state");
}
static int start_mount(const char *mount, char **command, unsigned timeout)
{
    if (beneath(mount, "/dev") || beneath(mount, "/proc") || beneath(mount, "/system") ||
        !strcmp(mount, "/data")) {
        errno = EINVAL;
        fail("choose a dedicated data mount directory");
    }
    char record[PATH_SIZE], lock[PATH_SIZE], log[PATH_SIZE];
    paths(mount, record, lock, log);
    int locked = lock_record(lock);
    struct mount_record old;
    struct qfuse_admin_status status;
    if (!read_record(record, mount, &old)) {
        if (!control(&old, QFUSE_ADMIN_STATUS, 0, &status,
                     !all_zero(old.instance, sizeof(old.instance)))) {
            print_status(&status);
            close(locked);
            errno = EEXIST;
            fail("mount already managed");
        }
        if (errno != ENOENT && errno != ESRCH && errno != ECONNRESET)
            fail("existing instance is unavailable; refusing replacement");
    } else if (errno != ENOENT)
        fail("invalid mount state; inspect it before replacing");
    struct stat st;
    if (lstat(mount, &st)) {
        if (errno != ENOENT || mkdir(mount, 0700))
            fail("create mount directory");
    } else if (!S_ISDIR(st.st_mode)) {
        errno = EINVAL;
        fail("mount directory must not be a symlink or file");
    }
    int fd = secure_open(record, O_RDWR | O_CREAT | O_TRUNC);
    if (fd == -1)
        fail("open mount state");
    strcpy(starting_record, record);
    int output = secure_open(log, O_WRONLY | O_CREAT | O_TRUNC);
    if (output == -1)
        fail("open private mount log");
    int adverts[2];
    if (pipe(adverts) || fcntl(adverts[0], F_SETFL, O_NONBLOCK))
        fail("startup channel");
    pid_t child = fork();
    if (child == -1)
        fail("fork");
    if (!child) {
        umask(child_umask);
        close(adverts[0]);
        char descriptor[32];
        snprintf(descriptor, sizeof(descriptor), "%d", adverts[1]);
        setenv("QFUSE_ADMIN_FD", descriptor, 1);
        if (setsid() == -1)
            _exit(126);
        int input = open("/dev/null", O_RDONLY);
        if (input != -1) {
            dup2(input, STDIN_FILENO);
            close(input);
        }
        dup2(output, STDOUT_FILENO);
        dup2(output, STDERR_FILENO);
        close(output);
        execvp(command[0], command);
        _exit(127);
    }
    close(adverts[1]);
    close(output);
    starting_child = child;
    strcpy(starting_record, record);
    struct mount_record r;
    memset(&r, 0, sizeof(r));
    r.magic = RECORD_MAGIC;
    r.version = 1;
    r.pid = child;
    strcpy(r.mount, mount);
    save_record(fd, &r);
    struct qfuse_admin_advert advert;
    size_t got = 0;
    int discovered = 0;
    printf("Starting background filesystem at %s...\n", mount);
    uint64_t end = now() + (uint64_t)timeout * 1000000000ULL;
    while (now() < end && !interrupted) {
        int code;
        pid_t exited = waitpid(child, &code, WNOHANG);
        if (exited == child) {
            starting_child = 0;
            fprintf(stderr, "qfuse-mount: filesystem exited; see %s\n", log);
            close(fd);
            close(locked);
            return 1;
        }
        if (exited == -1)
            fail("waitpid");
        if (!discovered) {
            ssize_t n = read(adverts[0], (char *)&advert + got, sizeof(advert) - got);
            if (n > 0)
                got += (size_t)n;
            if (got == sizeof(advert)) {
                if (advert.magic != QFUSE_ADMIN_MAGIC || advert.version != QFUSE_ADMIN_VERSION ||
                    advert.pid != (uint32_t)child || !advert.channel ||
                    all_zero(advert.instance, sizeof(advert.instance))) {
                    errno = EPROTO;
                    fail("invalid startup identity");
                }
                r.channel = advert.channel;
                memcpy(r.instance, advert.instance, sizeof(r.instance));
                save_record(fd, &r);
                discovered = 1;
                close(adverts[0]);
            }
        }
        if (discovered && !control(&r, QFUSE_ADMIN_STATUS, 0, &status, 1) &&
            status.phase == QFUSE_ADMIN_SERVING) {
            memcpy(r.instance, status.instance, sizeof(r.instance));
            save_record(fd, &r);
            starting_child = 0;
            starting_record[0] = 0;
            close(fd);
            close(locked);
            print_status(&status);
            return 0;
        }
        pause_short();
    }
    close(fd);
    close(locked);
    errno = interrupted ? EINTR : ETIMEDOUT;
    fail("startup did not become ready");
    return 1;
}
static int manage(const char *action, const char *mount, int force)
{
    char record[PATH_SIZE], lock[PATH_SIZE], log[PATH_SIZE];
    paths(mount, record, lock, log);
    int locked = -1;
    if (!strcmp(action, "stop"))
        locked = lock_record(lock);
    struct mount_record r;
    if (read_record(record, mount, &r))
        fail("no valid managed mount");
    if (all_zero(r.instance, sizeof(r.instance))) {
        errno = ESTALE;
        fail("startup state was interrupted; inspect the process before adopting it");
    }
    struct qfuse_admin_status status;
    if (control(&r, QFUSE_ADMIN_STATUS, 0, &status, 1))
        fail("instance unavailable or changed");
    if (!strcmp(action, "status")) {
        print_status(&status);
        return 0;
    }
    unsigned flags = force ? QFUSE_ADMIN_FORCE : 0;
    if (control(&r, QFUSE_ADMIN_STOP, flags, &status, 1))
        fail("stop refused (close files or use --force)");
    uint64_t end = now() + 3000000000ULL;
    while (now() < end && !interrupted) {
        if (control(&r, QFUSE_ADMIN_STATUS, 0, &status, 1)) {
            if (errno == ENOENT || errno == ESRCH || errno == ECONNRESET) {
                unlink(record);
                close(locked);
                printf("Stopped %s\n", mount);
                return 0;
            }
            if (errno == ESTALE)
                fail("instance changed during shutdown");
        }
        pause_short();
    }
    if (!force) {
        close(locked);
        errno = EBUSY;
        fail("shutdown pending; use stop --force for a stalled callback");
    }
    if (control(&r, QFUSE_ADMIN_KILL, status.group_leader ? QFUSE_ADMIN_GROUP : 0, &status, 1))
        fail("force stop refused");
    end = now() + 2000000000ULL;
    while (now() < end) {
        if (control(&r, QFUSE_ADMIN_STATUS, 0, &status, 1) &&
            (errno == ENOENT || errno == ESRCH || errno == ECONNRESET || errno == ESTALE)) {
            unlink(record);
            close(locked);
            printf("Stopped %s (forced)\n", mount);
            return 0;
        }
        pause_short();
    }
    close(locked);
    errno = ETIMEDOUT;
    fail("force stop requested but process disappearance is unconfirmed; state retained");
    return 1;
}
static void usage(void)
{
    fputs("Usage:\n  qfuse-mount sshfs [-F config] [--rw] REMOTE MOUNT\n  qfuse-mount start MOUNT "
          "-- PROGRAM ARG...\n  qfuse-mount status MOUNT\n  qfuse-mount stop [--force] MOUNT\n",
          stderr);
}
int main(int argc, char **argv)
{
    child_umask = umask(077);
    setvbuf(stdout, NULL, _IOLBF, 0);
    atexit(cleanup_start);
    struct sigaction action = {0};
    action.sa_handler = note_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        usage();
        return 0;
    }
    if (geteuid() != 0) {
        errno = EPERM;
        fail("root is required");
    }
    if (argc < 3) {
        usage();
        return 2;
    }
    if (!strcmp(argv[1], "status") || !strcmp(argv[1], "stop")) {
        int force = argc == 4 && !strcmp(argv[2], "--force");
        const char *mount = argv[argc - 1];
        if ((argc != 3 && (!force || strcmp(argv[1], "stop"))) || !clean_absolute(mount)) {
            usage();
            return 2;
        }
        return manage(argv[1], mount, force);
    }
    if (!strcmp(argv[1], "start")) {
        if (argc < 5 || strcmp(argv[3], "--") || !clean_absolute(argv[2])) {
            usage();
            return 2;
        }
        return start_mount(argv[2], &argv[4], 20);
    }
    if (!strcmp(argv[1], "sshfs")) {
        const char *config = NULL;
        int rw = 0, i = 2;
        while (i < argc && argv[i][0] == '-') {
            if (!strcmp(argv[i], "--rw")) {
                rw = 1;
                i++;
            } else if (!strcmp(argv[i], "-F") && i + 1 < argc) {
                config = argv[i + 1];
                i += 2;
            } else {
                usage();
                return 2;
            }
        }
        if (argc - i != 2 || !clean_absolute(argv[i + 1]) || !strchr(argv[i], ':')) {
            usage();
            return 2;
        }
        char binary[PATH_SIZE];
        const char *slash = strrchr(argv[0], '/');
        if (slash) {
            int n = snprintf(binary, sizeof(binary), "%.*s/qfuse-sshfs", (int)(slash - argv[0]),
                             argv[0]);
            if (n >= PATH_SIZE) {
                errno = ENAMETOOLONG;
                fail("program path");
            }
        } else
            strcpy(binary, "qfuse-sshfs");
        char *command[12];
        int n = 0;
        command[n++] = binary;
        command[n++] = argv[i];
        command[n++] = argv[i + 1];
        command[n++] = "-f";
        if (config) {
            command[n++] = "-F";
            command[n++] = (char *)config;
        }
        command[n++] = "-o";
        command[n++] = rw ? "rw,dir_cache=no,no_readahead,sync_readdir"
                          : "ro,dir_cache=no,no_readahead,sync_readdir";
        command[n] = NULL;
        return start_mount(argv[i + 1], command, 20);
    }
    usage();
    return 2;
}
