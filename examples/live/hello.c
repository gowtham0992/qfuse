/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#ifndef LIVE_MESSAGE
#define LIVE_MESSAGE "First build from the laptop"
#endif
int main(int argc, char **argv)
{
#if defined(__aarch64__)
    puts("QNX aarch64le — executing from the laptop's shared build folder");
#else
    puts("QNX x86_64 — executing from the laptop's shared build folder");
#endif
    puts(LIVE_MESSAGE);
    if (argc > 1) {
        FILE *file = fopen(argv[1], "r");
        if (!file) {
            perror("open live settings");
            return 1;
        }
        char line[512];
        while (fgets(line, sizeof(line), file))
            fputs(line, stdout);
        int failed = ferror(file);
        fclose(file);
        if (failed)
            return 1;
    }
    return 0;
}
