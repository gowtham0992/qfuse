/* SPDX-License-Identifier: MIT
 * Optional, non-setuid root launcher for an enforcing QNX security policy.
 * Exec in place so qfuse-mount retains its authenticated child PID/group.
 */
#include <secpol/secpol.h>
#include <sys/types.h>
#include <grp.h>
#include <spawn.h>
extern char **environ;
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    if (argc<4 || argv[2][0]!='-' || argv[2][1]!='-' || argv[2][2] || geteuid()!=0) {
        fputs("Usage (root, enforcing policy): qfuse-service-exec UID -- PROGRAM [ARGS...]\n",stderr);
        return 126;
    }
    char *end; errno=0;
    unsigned long parsed=strtoul(argv[1],&end,10);
    uid_t uid=(uid_t)parsed; gid_t gid=(gid_t)parsed;
    if (errno || !*argv[1] || *end || !parsed || parsed>INT_MAX || (unsigned long)uid!=parsed || (unsigned long)gid!=parsed) {
        fputs("qfuse-service-exec: UID must be a nonzero numeric service identity\n",stderr);
        return 126;
    }
    if (setgroups(0,NULL)) {
        perror("qfuse-service-exec: clear supplementary groups"); return 126;
    }
    if (secpol_transition_type(NULL,"qfuse_service",0)) {
        perror("qfuse-service-exec: policy transition"); return 126;
    }
    if (setgid(gid) || setuid(uid) || getuid()!=uid || geteuid()!=uid || getgid()!=gid || getegid()!=gid) {
        perror("qfuse-service-exec: service identity transition"); return 126;
    }
    posix_spawnattr_t attr;
    int error=posix_spawnattr_init(&attr);
    if (!error) {
        error=posix_spawnattr_setxflags(&attr,POSIX_SPAWN_EXEC);
        if (!error && secpol_posix_spawnattr_settypeid(NULL,&attr,"qfuse_exec",0))
            error=errno ? errno : EPERM;
        if (!error) error=posix_spawnp(NULL,argv[3],NULL,&attr,&argv[3],environ);
        posix_spawnattr_destroy(&attr);
    }
    errno=error ? error : EIO;
    perror("qfuse-service-exec: policy exec");
    return 127;
}
