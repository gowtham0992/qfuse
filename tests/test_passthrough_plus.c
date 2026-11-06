/* SPDX-License-Identifier: MIT
 * The unchanged upstream --plus mode must fail explicitly: QFuse requests
 * ordinary directory entries and does not negotiate complete PLUS metadata.
 */
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static pid_t server;
static void stop(void) { if (server > 0) { kill(server, SIGKILL); waitpid(server, NULL, 0); server=0; } }
static void timeout(int n) { (void)n; if (server>0) kill(server,SIGKILL); _exit(124); }
#define CHECK(x) do { if (!(x)) { perror(#x); exit(1); } } while (0)
int main(void)
{
    atexit(stop); signal(SIGALRM,timeout); alarm(60);
    server=fork(); CHECK(server>=0);
    if (!server) {
        execl("/data/qfuse/qfuse-passthrough", "passthrough", "-f", "--plus", "/qfuse-plus", NULL);
        _exit(127);
    }
    struct stat st;
    int ready=0;
    for (int i=0;i<200;i++) {
        if (!stat("/qfuse-plus/data",&st)) { ready=1; break; }
        struct timespec pause={0,100000000}; nanosleep(&pause,NULL);
    }
    CHECK(ready);
    DIR *stream=opendir("/qfuse-plus/data");
    if (!stream) CHECK(errno==ENOTSUP);
    else {
        errno=0; struct dirent *entry=readdir(stream); int saved=errno;
        CHECK(!entry && saved==ENOTSUP); CHECK(closedir(stream)==0);
    }
    stop();
    server=fork(); CHECK(server>=0);
    if (!server) {
        execl("/data/qfuse/qfuse-passthrough", "passthrough", "-f", "/qfuse-plus", NULL);
        _exit(127);
    }
    ready=0;
    for (int i=0;i<200;i++) {
        if (!stat("/qfuse-plus/data",&st)) { ready=1; break; }
        struct timespec pause={0,100000000}; nanosleep(&pause,NULL);
    }
    CHECK(ready);
    /* Mirrored root contains the server's own mountpoint. Type discovery must
     * not issue a nested path request to that mount and deadlock the callback. */
    stream=opendir("/qfuse-plus"); CHECK(stream);
    int count=0; errno=0;
    while (readdir(stream)) count++;
    CHECK(count>0 && errno==0); CHECK(closedir(stream)==0);
    stop(); alarm(0);
    puts("PASS unchanged passthrough --plus: unsupported complete-attribute hint rejected with ENOTSUP; ordinary root listing completes without recursive lookup");
    return 0;
}
