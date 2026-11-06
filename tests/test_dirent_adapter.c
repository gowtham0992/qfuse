/* SPDX-License-Identifier: MIT */
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#define CHECK(x) do { if (!(x)) { perror(#x); return 1; } } while (0)
int main(void)
{
    char root[] = "/data/qfuse-dirent.XXXXXX", file[1024];
    CHECK(mkdtemp(root));
    snprintf(file, sizeof(file), "%s/regular", root);
    int fd = open(file, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    snprintf(file, sizeof(file), "%s/directory", root);
    CHECK(mkdir(file, 0700) == 0);
    snprintf(file, sizeof(file), "%s/link", root);
    CHECK(symlink("regular", file) == 0);
    snprintf(file, sizeof(file), "%s/dangling", root);
    CHECK(symlink("missing", file) == 0);
    snprintf(file, sizeof(file), "%s/fifo", root);
    CHECK(mkfifo(file, 0600) == 0);
    char longname[NAME_MAX + 1];
    long limit = pathconf(root, _PC_NAME_MAX);
    CHECK(limit > 0 && limit <= NAME_MAX);
    memset(longname, 'n', (size_t)limit); longname[limit] = 0;
    snprintf(file, sizeof(file), "%s/%s", root, longname);
    fd = open(file, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    DIR *first = opendir(root), *second = opendir(root);
    CHECK(first && second);
    /* Directory descriptor resolution must survive a different process cwd. */
    CHECK(chdir("/") == 0);
    struct dirent *held = readdir(first);
    CHECK(held);
    char held_name[NAME_MAX + 1]; strcpy(held_name, held->d_name);
    CHECK(readdir(second));
    CHECK(!strcmp(held_name, held->d_name));
    rewinddir(first);
    int count = 0, regular = 0, directory = 0, links = 0, fifo = 0, longest = 0;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(first))) {
        CHECK(entry->d_namlen == strlen(entry->d_name));
        CHECK(entry->d_reclen >= strlen(entry->d_name) + 1);
        if (!strcmp(entry->d_name, "regular")) { CHECK(entry->d_type == DT_REG); regular++; }
        else if (!strcmp(entry->d_name, "directory")) { CHECK(entry->d_type == DT_DIR); directory++; }
        else if (!strcmp(entry->d_name, "link") || !strcmp(entry->d_name, "dangling")) {
            CHECK(entry->d_type == DT_LNK); links++;
        } else if (!strcmp(entry->d_name, "fifo")) { /* This filesystem omits FIFO extras; unknown is valid, an invented
             * type is not. Other filesystems may supply the actual FIFO type. */
            CHECK(entry->d_type == DT_UNKNOWN || entry->d_type == DT_FIFO); fifo++; }
        else if (!strcmp(entry->d_name, longname)) { CHECK(entry->d_type == DT_REG); longest++; }
        count++;
    }
    CHECK(errno == 0 && count == 8 && regular == 1 && directory == 1 && links == 2 && fifo == 1 && longest == 1);
    CHECK(closedir(first) == 0 && closedir(second) == 0);
    const char *names[] = {"regular", "link", "dangling", "fifo", longname};
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); i++) {
        snprintf(file, sizeof(file), "%s/%s", root, names[i]); CHECK(unlink(file) == 0);
    }
    snprintf(file, sizeof(file), "%s/directory", root); CHECK(rmdir(file) == 0);
    CHECK(rmdir(root) == 0);
    puts("PASS dirent adapter: native metadata types, dangling symlink, optional FIFO type, filesystem maximum name, independent streams, changed cwd, clean EOF");
    return 0;
}
