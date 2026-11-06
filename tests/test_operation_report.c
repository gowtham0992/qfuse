/* SPDX-License-Identifier: MIT
 * Named differential cases. Only descriptors and filesystem identity are
 * normalized: descriptor numbers are private capabilities, and mount device/
 * inode IDs intentionally differ between two independently created trees.
 */
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>

#define LIMIT 96
struct observation {
    const char *name;
    int64_t returned;
    int error;
    char value[256];
};
static struct observation expected[LIMIT];
static size_t expected_count, position;
static int mounted_run, mismatches;
static pid_t server;
static char fixture[512];
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL named cases line %d: %s errno=%d (%s)\n", __LINE__, #x, errno,   \
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
    _exit(124);
}
static void path(char *out, size_t size, const char *root, const char *name)
{
    int n = snprintf(out, size, "%s/%s", root, name);
    CHECK(n > 0 && (size_t)n < size);
}
static void record(const char *name, int64_t returned, int descriptor, const char *value)
{
    int saved = returned < 0 ? errno : 0;
    struct observation actual = {
        .name = name, .returned = descriptor && returned >= 0 ? 0 : returned, .error = saved};
    if (value)
        snprintf(actual.value, sizeof(actual.value), "%s", value);
    CHECK(position < LIMIT);
    if (!mounted_run) {
        expected[position++] = actual;
        expected_count = position;
        return;
    }
    CHECK(position < expected_count);
    struct observation *native = &expected[position++];
    int same = !strcmp(native->name, name) && native->returned == actual.returned &&
               native->error == actual.error && !strcmp(native->value, actual.value);
    printf("QFUSE_CASE\t%s\t%lld\t%lld\t%d\t%d\t%s\t%s\t%s\n", name, (long long)native->returned,
           (long long)actual.returned, native->error, actual.error, native->value, actual.value,
           same ? "PASS" : "MISMATCH");
    if (!same)
        mismatches++;
}
static void stat_value(char out[256], const struct stat *st, int directory, int timestamps)
{
    int n = snprintf(out, 256, "mode=%o,uid=%u,gid=%u,nlink=%lu", (unsigned)st->st_mode,
                     (unsigned)st->st_uid, (unsigned)st->st_gid, (unsigned long)st->st_nlink);
    CHECK(n > 0 && n < 256);
    if (!directory) {
        int more = snprintf(out + n, 256 - n, ",size=%lld", (long long)st->st_size);
        CHECK(more > 0 && more < 256 - n);
        n += more;
    }
    if (timestamps) {
        int more = snprintf(out + n, 256 - n, ",atime=%lld.%09ld,mtime=%lld.%09ld",
                            (long long)st->st_atim.tv_sec, st->st_atim.tv_nsec,
                            (long long)st->st_mtim.tv_sec, st->st_mtim.tv_nsec);
        CHECK(more > 0 && more < 256 - n);
    }
}
static void bytes_value(char out[256], const unsigned char *bytes, ssize_t size)
{
    CHECK(size <= 100);
    size_t used = 0;
    out[0] = 0;
    for (ssize_t i = 0; i < size; i++) {
        int n = snprintf(out + used, 256 - used, "%02x", bytes[i]);
        CHECK(n == 2);
        used += 2;
    }
}
static void stat_case(const char *name, const char *file, int fd, int follow, int directory,
                      int timestamps)
{
    struct stat st;
    char value[256] = "";
    errno = 0;
    int r = fd >= 0 ? fstat(fd, &st) : follow ? stat(file, &st) : lstat(file, &st);
    int saved = errno;
    if (!r)
        stat_value(value, &st, directory, timestamps);
    errno = saved;
    record(name, r, 0, value);
}
#define SCALAR(name, expression)                                                                   \
    do {                                                                                           \
        errno = 0;                                                                                 \
        long long r = (expression);                                                                \
        record(name, r, 0, NULL);                                                                  \
        CHECK(r == 0);                                                                             \
    } while (0)
static void exercise(const char *root)
{
    char file[1200], renamed[1200], dir[1200], child[1200], link[1200], value[256];
    path(file, sizeof(file), root, "file");
    path(renamed, sizeof(renamed), root, "renamed");
    path(dir, sizeof(dir), root, "dir");
    path(child, sizeof(child), root, "dir/child");
    path(link, sizeof(link), root, "link");
    errno = 0;
    int fd = open(file, O_CREAT | O_EXCL | O_RDWR, 0640);
    record("01.create_exclusive", fd, 1, NULL);
    CHECK(fd >= 0);
    stat_case("02.fstat_created", NULL, fd, 0, 0, 0);
    errno = 0;
    ssize_t n = write(fd, "abcdefgh", 8);
    record("03.write_initial", n, 0, NULL);
    CHECK(n == 8);
    errno = 0;
    off_t offset = lseek(fd, 0, SEEK_CUR);
    record("04.seek_after_write", offset, 0, NULL);
    CHECK(offset == 8);
    errno = 0;
    n = pwrite(fd, "XY", 2, 2);
    record("05.pwrite", n, 0, NULL);
    CHECK(n == 2);
    errno = 0;
    offset = lseek(fd, 0, SEEK_CUR);
    record("06.pwrite_preserves_offset", offset, 0, NULL);
    CHECK(offset == 8);
    unsigned char bytes[100] = {0};
    errno = 0;
    n = pread(fd, bytes, 8, 0);
    int saved = errno;
    bytes_value(value, bytes, n);
    errno = saved;
    record("07.pread_content", n, 0, value);
    CHECK(n == 8);
    stat_case("08.fstat_written_size", NULL, fd, 0, 0, 0);
    SCALAR("09.truncate_shrink", ftruncate(fd, 4));
    stat_case("10.fstat_shrunk", NULL, fd, 0, 0, 0);
    SCALAR("11.truncate_extend", ftruncate(fd, 12));
    memset(bytes, 1, sizeof(bytes));
    errno = 0;
    n = pread(fd, bytes, 8, 4);
    saved = errno;
    bytes_value(value, bytes, n);
    errno = saved;
    record("12.extended_zero_bytes", n, 0, value);
    CHECK(n == 8);
    SCALAR("13.fchmod", fchmod(fd, 0600));
    stat_case("14.fstat_changed_mode", NULL, fd, 0, 0, 0);
    SCALAR("15.fsync", fsync(fd));
    SCALAR("16.close_written", close(fd));
    errno = 0;
    int duplicate = open(file, O_CREAT | O_EXCL | O_WRONLY, 0600);
    record("17.exclusive_existing_error", duplicate, 1, NULL);
    CHECK(duplicate == -1);
    errno = 0;
    fd = open(file, O_WRONLY | O_APPEND);
    record("18.open_append", fd, 1, NULL);
    CHECK(fd >= 0);
    errno = 0;
    n = write(fd, "!", 1);
    record("19.append", n, 0, NULL);
    CHECK(n == 1);
    SCALAR("20.close_append", close(fd));
    stat_case("21.stat_appended", file, -1, 1, 0, 0);
    SCALAR("22.truncate_path", truncate(file, 3));
    struct timespec times[2] = {{1234567890, 123456000}, {1234567891, 987654000}};
    SCALAR("23.utimensat", utimensat(AT_FDCWD, file, times, 0));
    stat_case("24.stat_exact_timestamps", file, -1, 1, 0, 1);
    SCALAR("25.rename", rename(file, renamed));
    stat_case("26.stat_old_name_error", file, -1, 1, 0, 0);
    SCALAR("27.mkdir", mkdir(dir, 0750));
    errno = 0;
    int r = mkdir(dir, 0750);
    record("28.mkdir_existing_error", r, 0, NULL);
    CHECK(r == -1);
    stat_case("29.stat_directory", dir, -1, 1, 1, 0);
    errno = 0;
    fd = open(child, O_CREAT | O_EXCL | O_WRONLY, 0600);
    record("30.create_child", fd, 1, NULL);
    CHECK(fd >= 0);
    SCALAR("31.close_child", close(fd));
    errno = 0;
    r = rmdir(dir);
    record("32.rmdir_nonempty_error", r, 0, NULL);
    CHECK(r == -1);
    SCALAR("33.unlink_child", unlink(child));
    SCALAR("34.rmdir_empty", rmdir(dir));
    SCALAR("35.symlink_relative", symlink("renamed", link));
    stat_case("36.lstat_link", link, -1, 0, 0, 0);
    memset(bytes, 0, sizeof(bytes));
    errno = 0;
    n = readlink(link, (char *)bytes, 100);
    saved = errno;
    bytes_value(value, bytes, n);
    errno = saved;
    record("37.readlink", n, 0, value);
    CHECK(n == 7);
    memset(bytes, 0, sizeof(bytes));
    errno = 0;
    n = readlink(link, (char *)bytes, 3);
    saved = errno;
    bytes_value(value, bytes, n);
    errno = saved;
    record("38.readlink_short_buffer", n, 0, value);
    CHECK(n == 3);
    errno = 0;
    fd = open(link, O_RDONLY);
    record("39.open_follow_link", fd, 1, NULL);
    CHECK(fd >= 0);
    errno = 0;
    n = read(fd, bytes, 3);
    saved = errno;
    bytes_value(value, bytes, n);
    errno = saved;
    record("40.read_followed_content", n, 0, value);
    CHECK(n == 3);
    errno = 0;
    n = pread(fd, bytes, 1, -1);
    record("41.pread_negative_error", n, 0, NULL);
    CHECK(n == -1);
    SCALAR("42.close_followed", close(fd));
    errno = 0;
    fd = open(link, O_RDONLY | O_NOFOLLOW);
    record("43.nofollow_error", fd, 1, NULL);
    CHECK(fd == -1);
    SCALAR("44.chmod_path", chmod(renamed, 0640));
    SCALAR("45.chown_self", chown(renamed, getuid(), getgid()));
    stat_case("46.stat_owner_mode", renamed, -1, 1, 0, 0);
    struct statvfs fs;
    errno = 0;
    r = statvfs(root, &fs);
    saved = errno;
    value[0] = 0;
    if (!r)
        snprintf(value, sizeof(value), "bsize=%lu,frsize=%lu,namemax=%lu", fs.f_bsize, fs.f_frsize,
                 fs.f_namemax);
    errno = saved;
    record("47.statvfs_geometry", r, 0, value);
    CHECK(r == 0);
    SCALAR("48.unlink_link", unlink(link));
    SCALAR("49.unlink_file", unlink(renamed));
    errno = 0;
    fd = open(renamed, O_RDONLY);
    record("50.open_missing_error", fd, 1, NULL);
    CHECK(fd == -1);
    SCALAR("51.rmdir_tree", rmdir(root));
}
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGALRM, timeout);
    alarm(180);
    atexit(stop);
    umask(0022);
    strcpy(fixture, "/data/qfuse-report.XXXXXX");
    CHECK(mkdtemp(fixture));
    char native[600], backing[600], view[1000];
    path(native, sizeof(native), fixture, "native");
    path(backing, sizeof(backing), fixture, "backing");
    CHECK(!mkdir(native, 0700) && !mkdir(backing, 0700));
    snprintf(view, sizeof(view), "/qfuse-report%s/backing", fixture);
    server = fork();
    CHECK(server >= 0);
    if (!server) {
        const char *bin = getenv("QFUSE_PASSTHROUGH_BINARY");
        execl(bin ? bin : "/data/qfuse/qfuse-passthrough", "passthrough", "-f", "/qfuse-report",
              NULL);
        _exit(127);
    }
    for (int i = 0; i < 200; i++) {
        struct stat st;
        if (!stat(view, &st))
            break;
        int status;
        if (waitpid(server, &status, WNOHANG) == server) {
            server = 0;
            CHECK(!"server exited early");
        }
        CHECK(i < 199);
        struct timespec pause = {0, 100000000};
        nanosleep(&pause, NULL);
    }
    exercise(native);
    position = 0;
    mounted_run = 1;
    puts("QFUSE_CASE_HEADER\tname\tnative_result\tmounted_result\tnative_errno\tmounted_"
         "errno\tnative_value\tmounted_value\tstatus");
    exercise(view);
    CHECK(position == expected_count && expected_count >= 40);
    stop();
    CHECK(rmdir(fixture) == 0);
    alarm(0);
    printf("QFUSE_CASE_SUMMARY\t%zu\t%d\n", expected_count, mismatches);
    CHECK(!mismatches);
    puts("PASS named differential report: distinct syscall cases, exact semantic results/errno and "
         "deterministic metadata/content");
    return 0;
}
