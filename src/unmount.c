/* SPDX-License-Identifier: MIT */
#include <sys/mount.h>
#include <qfuse/control.h>
#include <fcntl.h>
#include <sys/neutrino.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    int force = argc == 3 && (!strcmp(argv[1], "-f") || !strcmp(argv[1], "--force"));
    const char *path = force ? argv[2] : argc == 2 ? argv[1] : NULL;
    if (!path || path[0] != '/' || !path[1]) {
        fprintf(stderr, "Usage: qfuse-unmount [-f] /absolute/mountpoint\n"
                        "  -f closes active filesystem handles; clients may receive I/O errors.\n");
        return 2;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "qfuse-unmount: root is required\n");
        return 1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("qfuse-unmount");
        return 1;
    }
    if (getenv("QFUSE_TRACE")) {
        struct _server_info target;
        if (ConnectServerInfo(0, fd, &target) != -1)
            fprintf(stderr, "unmount control fd=%d server=%d\n", fd, target.pid);
    }
    struct qfuse_mount_info info = {0};
    int status = devctl(fd, QFUSE_CONTROL_GET_INFO, &info, sizeof(info), NULL);
    close(fd);
    if (status || info.magic != QFUSE_CONTROL_MAGIC || info.version != 1 ||
        !memchr(info.mountpoint, 0, sizeof(info.mountpoint)) || strcmp(info.mountpoint, path)) {
        fprintf(stderr,
                "qfuse-unmount: path is not a QFuse mount root (status=%d magic=%x version=%u)\n",
                status, info.magic, info.version);
        return 1;
    }
    if (umount(path, force ? _MOUNT_FORCE : 0)) {
        perror("qfuse-unmount");
        return 1;
    }
    return 0;
}
