/* SPDX-License-Identifier: MIT
 * One-shot lab measurement; no latency acceptance threshold or cache flushing.
 * Source: 10 directories, 500 files each, 97 deterministic bytes per file.
 */
#define _QNX_SOURCE
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char native[] = "/data/qfuse-measure.XXXXXX";
static int owned;
#define CHECK(x) do { if (!(x)) { perror(#x); exit(1); } } while (0)
static void path(char *out, size_t size, const char *root, int directory, int file)
{
    int n = file < 0 ? snprintf(out, size, "%s/d%02d", root, directory)
                     : snprintf(out, size, "%s/d%02d/file-%04d.bin", root, directory, file);
    CHECK(n > 0 && (size_t)n < size);
}
static void cleanup(void)
{
    if (!owned) return;
    char name[1024];
    for (int d = 0; d < 10; ++d) {
        for (int f = 0; f < 500; ++f) {
            path(name, sizeof(name), native, d, f); unlink(name);
        }
        path(name, sizeof(name), native, d, -1); rmdir(name);
    }
    rmdir(native);
}
static uint64_t now(void)
{
    struct timespec t; CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
static void create_native(void)
{
    CHECK(mkdtemp(native)); owned = 1;
    printf("NATIVE_ROOT=%s\n", native);
    char name[1024]; unsigned char data[97];
    for (int d = 0; d < 10; ++d) {
        path(name, sizeof(name), native, d, -1); CHECK(!mkdir(name, 0755));
        for (int f = 0; f < 500; ++f) {
            for (size_t i = 0; i < sizeof(data); ++i)
                data[i] = (unsigned char)(i * 37u + (d * 500u + f) * 19u);
            path(name, sizeof(name), native, d, f);
            int fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0644); CHECK(fd >= 0);
            CHECK(write(fd, data, sizeof(data)) == sizeof(data)); CHECK(!close(fd));
        }
    }
}
static uint64_t list_tree(const char *root)
{
    uint64_t start = now(); pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0) _exit(126);
        close(fd); execlp("ls", "ls", "-lR", root, (char *)NULL); _exit(127);
    }
    int status; CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return now() - start;
}
static uint64_t read_tree(const char *root)
{
    uint64_t start = now(); char name[1024]; unsigned char data[98];
    for (int d = 0; d < 10; ++d) {
        for (int f = 0; f < 500; ++f) {
            path(name, sizeof(name), root, d, f);
            int fd = open(name, O_RDONLY); CHECK(fd >= 0);
            size_t used = 0;
            while (used < sizeof(data)) {
                ssize_t n = read(fd, data + used, sizeof(data) - used); CHECK(n >= 0);
                if (!n) break;
                used += (size_t)n;
            }
            CHECK(used == 97);
            for (size_t i = 0; i < used; ++i)
                CHECK(data[i] == (unsigned char)(i * 37u + (d * 500u + f) * 19u));
            CHECK(!close(fd));
        }
    }
    return now() - start;
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 2 || argv[1][0] != '/') {
        fprintf(stderr, "Usage: measure-share /absolute/remote/fixture\n"); return 2;
    }
    CHECK(!atexit(cleanup)); create_native();
    const char *labels[] = {"native", "sshfs"};
    const char *roots[] = {native, argv[1]};
    for (int i = 0; i < 2; ++i)
        printf("{\"operation\":\"ls-lR\",\"source\":\"%s\",\"elapsed_ns\":%llu}\n",
               labels[i], (unsigned long long)list_tree(roots[i]));
    for (int i = 0; i < 2; ++i)
        printf("{\"operation\":\"verified-read-loop\",\"source\":\"%s\",\"files\":5000,\"bytes\":485000,\"elapsed_ns\":%llu}\n",
               labels[i], (unsigned long long)read_tree(roots[i]));
    puts("PASS measured both trees; every file's length, bytes, EOF and close verified");
    return 0;
}
