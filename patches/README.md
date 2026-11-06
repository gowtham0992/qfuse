# Application portability patches

Passthrough builds without a source patch using the optional directory-entry
adapter described in the [porting guide](../docs/guide/porting.md).

`sshfs-qnx.patch` changes one preprocessor guard in SSHFS 3.7.3: QNX skips
setting FD_CLOEXEC on the Linux FUSE transport descriptor. QFuse uses native
resource-manager IPC and has no such descriptor. The callbacks and SFTP
protocol code remain upstream. This patch is GPL-2.0-only.

`bindfs-qnx.patch` selects `O_DIRECTORY` when opening bindfs 1.18.4's backing
folder on QNX, for its directory-descriptor operations. Bindfs also uses the
explicit optional directory-entry adapter. This patch is GPL-2.0-or-later,
matching bindfs. Sources and license texts are fetched unchanged and hash-locked;
patches are applied to private build copies.
