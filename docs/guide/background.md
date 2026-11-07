# Motivation, prior art and evidence limits

QFuse's verified use is running a tested high-level FUSE subset on QNX 8.0,
including SSHFS and the optional bindfs port. The source record below motivates
that work; it is not evidence that every FUSE filesystem works or that a current
outside developer has adopted QFuse.

## What changed in QNX 8.0

The [official networking discontinuation notice](https://www.qnx.com/developers/docs/relnotes8.0/com.qnx.doc.discontinuation_notice/topic/networking.html)
lists `fs-cifs` as discontinued and gives the alternative “Use a compatible
third-party SMBv3 CIFS.” It also lists Qnet as discontinued following removal of
io-pkt. The [filesystem notice](https://www.qnx.com/developers/docs/relnotes8.0/com.qnx.doc.discontinuation_notice/topic/filesystems.html)
lists the legacy Windows NT and Apple Macintosh HFS filesystems as discontinued.
It does not list exFAT among those removed filesystems.

QNX's [7.1 host-share instructions](https://www.qnx.com/developers/docs/7.1/com.qnx.doc.adas.getting_started.overview/topic/mount_fs_to_image.html)
describe sharing a host folder with a target, including Windows CIFS and
Linux/macOS NFS exports. That establishes an existing workflow, rather than a new
problem invented for this port. QNX 8 still has an [NFS client](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.user_guide/topic/fsystems_NFS.html),
and the [Toolkit](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.qnxtoolkit.user_guide/topic/launching_an_application.html)
builds and launches target programs.

SSHFS offers another transport where authorized SSH **and SFTP** access exists.
A machine merely running an SSH server is insufficient if its SFTP subsystem or
filesystem permissions do not permit access. The tests used Linux SFTP servers;
Windows/macOS SFTP-server interoperability and QNX-to-QNX SSHFS mounts were not
verified. SSHFS is not an SMB client or a replacement for Qnet's wider services.

## The earlier QNX FUSE project

The [QNX Community FUSE Project](https://community.qnx.com/sf/projects/qnx_community_fuse_project/)
was created in 2009 and described using QNX's resource-manager interface instead
of adding a FUSE kernel module. Its [wiki](https://community.qnx.com/sf/wiki/do/viewPage/projects.qnx_community_fuse_project/wiki/HomePage)
names the tree `qfuse`, identifies libfuse 2.7.4, and shows QNX 6.3.2 examples
with ntfs-3g, ext2fuse and zfsfuse. This is direct prior art for both the mapping
idea and the name. The present independently implemented QNX 8.0 layer does not
claim to be the first FUSE-on-QNX work or an official continuation of that tree.

## What the demand evidence establishes

A [2019 OpenQNX discussion](https://forums.openqnx.com/t/topic/8096) records a
developer experimenting with exFAT through the earlier FUSE library and reporting
lifecycle/unmount concerns. It is a historical first-person report, not a
benchmark or evidence of QFuse use. Upstream [ntfs-3g's README](https://github.com/tuxera/ntfs-3g/blob/edge/README)
lists QNX among its platforms without establishing compatibility with this
QNX 8.0 library.

**Inference:** a source-compatibility layer may help developers evaluate FUSE
implementations for filesystem access that stock QNX 8 no longer supplies.
That possibility does not establish present-day demand for this particular API
subset. ntfs-3g, exFAT, SMB and HFS backends remain unported and untested here;
the discontinuation notice alone does not establish their feasibility.

The supported claim today is narrower: selected existing FUSE callback code runs
as QNX 8.0 resource managers, and SSHFS supplies a verified SSH/SFTP host-share
workflow. Independent adoption, maintenance interest, real-world performance and
additional filesystem compatibility remain separate questions.
