/* SPDX-License-Identifier: MIT
 * Run only in an enforcing QNX 8.0 policy lab as the dedicated service UID.
 * These are negative permission probes, not a general sandbox audit.
 */
#include <sys/procmgr.h>
#include <sys/pathmgr.h>
#include <sys/stat.h>
#include <sys/neutrino.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL service permissions line %d errno=%d\n",__LINE__,errno); return 1; } } while (0)
int main(int argc, char **argv)
{
    CHECK(argc==2 && geteuid()==65534 && getegid()==65534);
    errno=0; CHECK(setuid(0)==-1 && errno==EPERM);
    errno=0; int fd=open(argv[1],O_RDONLY);
    if(fd>=0) close(fd);
    CHECK(fd==-1 && errno==EACCES);
    /* A locked ability-change request may be ignored with success unless
     * procnto enforces lock failures. Probe actual operations after attempting
     * escalation instead of treating the request's return as authority. */
    (void)procmgr_ability(0, PROCMGR_ADN_NONROOT|PROCMGR_AOP_ALLOW|PROCMGR_AID_MEM_PHYS, PROCMGR_AID_EOL);
    (void)procmgr_ability(0, PROCMGR_ADN_NONROOT|PROCMGR_AOP_ALLOW|PROCMGR_AID_IO, PROCMGR_AID_EOL);
    errno=0;
    void *mapping=mmap_device_memory(NULL,4096,PROT_READ,0,0x40000000);
    int saved_map=errno;
    if(mapping!=MAP_FAILED) munmap(mapping,4096);
    CHECK(mapping==MAP_FAILED && (saved_map==EPERM || saved_map==EACCES));
    errno=0; int result=ThreadCtl(_NTO_TCTL_IO_LEVEL,(void *)_NTO_IO_LEVEL_1);
    CHECK(result==-1 && errno==EPERM);
    errno=0; result=pathmgr_symlink("/", "/data/qfuse-forbidden-link");
    int saved=errno;
    if(!result) pathmgr_unlink("/data/qfuse-forbidden-link");
    CHECK(result==-1 && (saved==EPERM || saved==EACCES));
    puts("PASS restricted service: dedicated UID, root transition, private file, physical-memory/IO ability escalation and unauthorized path link denied");
    return 0;
}
