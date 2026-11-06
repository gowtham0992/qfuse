/* SPDX-License-Identifier: MIT */
#define FUSE_USE_VERSION 31
#include <fuse.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    struct rlimit limit = {0, 0};
    setrlimit(RLIMIT_CORE, &limit);
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0)
        return 1;
    char *region =
        mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (region == MAP_FAILED || mprotect(region + page, page, PROT_NONE))
        return 2;
    /* op_size permits a prefix of the operation table. No byte beyond it exists. */
    size_t size = sizeof(((struct fuse_operations *)0)->getattr);
    struct fuse_operations *ops = (void *)(region + page - size);
    memset(ops, 0, size);
    char *argv[] = {"abi", NULL};
    struct fuse_args args = FUSE_ARGS_INIT(1, argv);
    struct fuse *f = fuse_new(&args, ops, size, NULL);
    if (!f)
        return 3;
    fuse_destroy(f);
    munmap(region, (size_t)page * 2);
    puts("PASS library ABI: fuse_new reads only the supplied operation-table prefix");
    return 0;
}
