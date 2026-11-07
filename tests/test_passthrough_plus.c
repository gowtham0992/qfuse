/* SPDX-License-Identifier: MIT
 * Complete FUSE directory metadata maps to native QNX stat extras. Metadata
 * must never bypass the current caller's directory search permissions.
 */
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static pid_t server;
static char root[] = "/data/qfuse-dirplus.XXXXXX";
static void stop(void) { if (server > 0) { kill(server, SIGKILL); waitpid(server, NULL, 0); server=0; } }
static void cleanup(void)
{
    stop();
    char path[512];
    snprintf(path,sizeof(path),"%s/file",root); unlink(path);
    snprintf(path,sizeof(path),"%s/link",root); unlink(path);
    snprintf(path,sizeof(path),"%s/nox/child",root); unlink(path);
    snprintf(path,sizeof(path),"%s/nox",root); chmod(path,0700); rmdir(path);
    rmdir(root);
}
static void timeout(int n) { (void)n; if (server>0) kill(server,SIGKILL); _exit(124); }
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL dirplus line %d: %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while (0)
static const struct stat *extra_stat(struct dirent *entry)
{
    for (struct dirent_extra *e=_DEXTRA_FIRST(entry); _DEXTRA_VALID(e,entry); e=_DEXTRA_NEXT(e))
        if (e->d_type==_DTYPE_LSTAT && e->d_datalen>=sizeof(struct stat))
            return &((struct dirent_extra_stat *)e)->d_stat;
    return NULL;
}
static void launch(int plus)
{
    server=fork(); CHECK(server>=0);
    if (!server) {
        if (plus==1 || plus==3) {
            setenv(plus==1 ? "QFUSE_DIR_STAT_FULL" : "QFUSE_DIR_STAT_NULL", "1", 1);
            execl("/data/qfuse/qfuse-inflight","inflight","-f","/qfuse-plus",NULL);
        }
        if (plus==2) execl("/data/qfuse/qfuse-passthrough","passthrough","-f","--plus","/qfuse-plus",NULL);
        else execl("/data/qfuse/qfuse-passthrough","passthrough","-f","/qfuse-plus",NULL);
        _exit(127);
    }
    struct stat st;
    for (int i=0;i<200;i++) {
        if (!stat("/qfuse-plus/data",&st)) return;
        struct timespec pause={0,100000000}; nanosleep(&pause,NULL);
    }
    CHECK(!"server readiness");
}
int main(void)
{
    atexit(cleanup); signal(SIGALRM,timeout); alarm(60);
    CHECK(mkdtemp(root) && !chmod(root,0755));
    char native[512],view[768];
    snprintf(native,sizeof(native),"%s/file",root);
    int f=open(native,O_CREAT|O_EXCL|O_WRONLY,0640); CHECK(f>=0 && write(f,"content",7)==7 && !close(f));
    snprintf(native,sizeof(native),"%s/link",root); CHECK(!symlink("file",native));
    launch(1);
    snprintf(view,sizeof(view),"/qfuse-plus%s",root);
    DIR *stream=opendir(view); CHECK(stream);
    int flags=dircntl(stream,D_GETFLAG); CHECK(flags>=0 && !dircntl(stream,D_SETFLAG,flags|D_FLAG_STAT));
    int files=0,links=0; struct dirent *entry; errno=0;
    while ((entry=readdir(stream))) {
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        const struct stat *hint=extra_stat(entry); CHECK(hint);
        struct stat expected;
        char child[1024]; snprintf(child,sizeof(child),"%s/%s",view,entry->d_name);
        CHECK(!lstat(child,&expected));
        if (hint->st_dev!=expected.st_dev || hint->st_ino!=expected.st_ino || hint->st_mode!=expected.st_mode)
            fprintf(stderr,"extra %s dev=%llu ino=%llu mode=%o; stat dev=%llu ino=%llu mode=%o\n",entry->d_name,
                    (unsigned long long)hint->st_dev,(unsigned long long)hint->st_ino,hint->st_mode,
                    (unsigned long long)expected.st_dev,(unsigned long long)expected.st_ino,expected.st_mode);
        CHECK(hint->st_dev==expected.st_dev && hint->st_ino==expected.st_ino && hint->st_mode==expected.st_mode);
        CHECK(hint->st_uid==expected.st_uid && hint->st_gid==expected.st_gid && hint->st_nlink==expected.st_nlink);
        CHECK(hint->st_size==expected.st_size && hint->st_mtim.tv_sec==expected.st_mtim.tv_sec && hint->st_mtim.tv_nsec==expected.st_mtim.tv_nsec);
        files+=!strcmp(entry->d_name,"file"); links+=!strcmp(entry->d_name,"link"); errno=0;
    }
    CHECK(!errno && files==1 && links==1 && !closedir(stream));
    puts("PASS directory stat extras: virtual identity and file/symlink metadata match explicit lstat");
    snprintf(native,sizeof(native),"%s/nox",root); CHECK(!mkdir(native,0700));
    char child[1024]; snprintf(child,sizeof(child),"%s/child",native);
    f=open(child,O_CREAT|O_EXCL|O_WRONLY,0644); CHECK(f>=0 && write(f,"private",7)==7 && !close(f));
    CHECK(!chmod(native,0444)); snprintf(view,sizeof(view),"/qfuse-plus%s/nox",root);
    stream=opendir(view); CHECK(stream);
    flags=dircntl(stream,D_GETFLAG); CHECK(flags>=0 && !dircntl(stream,D_SETFLAG,flags|D_FLAG_STAT));
    CHECK(readdir(stream)); /* Prime the server snapshot as root. */
    pid_t reader=fork(); CHECK(reader>=0);
    if (!reader) {
        CHECK(!setgid(65534) && !setuid(65534)); rewinddir(stream);
        int found=0; errno=0;
        while ((entry=readdir(stream))) { CHECK(!extra_stat(entry)); found+=!strcmp(entry->d_name,"child"); errno=0; }
        CHECK(!errno && found==1 && !closedir(stream));
        struct stat st; snprintf(child,sizeof(child),"%s/child",view);
        CHECK(lstat(child,&st)==-1 && errno==EACCES); _exit(0);
    }
    int status; CHECK(waitpid(reader,&status,0)==reader && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(!closedir(stream));
    puts("PASS directory stat permissions: cached snapshot extras stripped for caller without search access");
    stop(); launch(2);
    snprintf(view,sizeof(view),"/qfuse-plus%s",root);
    stream=opendir(view); CHECK(stream);
    flags=dircntl(stream,D_GETFLAG); CHECK(flags>=0 && !dircntl(stream,D_SETFLAG,flags|D_FLAG_STAT));
    int partial=0; errno=0;
    while ((entry=readdir(stream))) { CHECK(!extra_stat(entry)); partial++; errno=0; }
    CHECK(partial>=3 && !errno && !closedir(stream));
    puts("PASS partial metadata: unchanged upstream --plus type/inode hints fall back without fabricated stat extras");
    stop(); launch(3);
    stream=opendir(view); CHECK(stream); errno=0;
    CHECK(!readdir(stream) && errno==EIO); CHECK(!closedir(stream));
    puts("PASS malformed full-stat hint rejected without dereferencing a null attribute pointer");
    stop(); launch(0);
    stream=opendir("/qfuse-plus"); CHECK(stream); int count=0; errno=0;
    while (readdir(stream)) count++;
    CHECK(count>0 && !errno && !closedir(stream));
    puts("PASS ordinary mirrored-root listing completes without recursive type lookup");
    cleanup(); alarm(0); return 0;
}
