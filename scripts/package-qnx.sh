#!/usr/bin/env bash
# Build/install a source-only static library SDK into a private staging prefix.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$project_root/scripts/target-config.sh"
sdk_root=${QNX_SDP_ROOT:-/home/qnx/qnx800}
source "$sdk_root/qnxsdp-env.sh" >/dev/null
build=${QFUSE_ARTIFACT_ROOT:-/home/qnx/qfuse-builds/latest/build/qnx}
prefix=${QFUSE_INSTALL_PREFIX:-/home/qnx/qfuse-install}
prefix=$(realpath -m -- "$prefix")
sdk_root=$(realpath -m -- "$sdk_root")
[[ $prefix = /* && $prefix != / && $prefix != "$sdk_root" && $prefix != "$sdk_root/"* ]] || exit 2
mkdir -p "$prefix/include/qfuse/compat" "$prefix/include/qfuse" "$prefix/lib/pkgconfig" "$prefix/include/fuse3" "$prefix/share/licenses/qfuse" "$prefix/bin"
"$QFUSE_BIN_PREFIX-ar" rcs "$prefix/lib/libfuse3.a" "$build/core.o" "$build/qnx_backend.o" "$build/fuse_opt.o" "$build/fuse_log.o"
cp "$project_root"/.cache/libfuse/include/*.h "$prefix/include/fuse3/"
cp "$project_root/include/qfuse/compat/dirent.h" "$prefix/include/qfuse/compat/"
cp "$project_root/include/qfuse/control.h" "$project_root/include/qfuse/admin.h" "$prefix/include/qfuse/"
cp "$build/libfuse_config.h" "$prefix/include/fuse3/"
cp "$build/qfuse-unmount" "$build/qfuse-mount" "$prefix/bin/"
if [[ -f $build/qfuse-sshfs ]]; then
    cp "$build/qfuse-sshfs" "$prefix/bin/"
    cp "$project_root/.cache/sshfs/COPYING" "$prefix/share/licenses/qfuse/sshfs-GPL2.txt"
fi
cp "$project_root/LICENSE" "$prefix/share/licenses/qfuse/QFuse-MIT"
cp "$project_root/.cache/libfuse/LGPL2.txt" "$prefix/share/licenses/qfuse/libfuse-LGPL2.txt"
cat > "$prefix/lib/pkgconfig/fuse3.pc" <<PC
prefix=$prefix
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: fuse3
Description: QFuse high-level FUSE API 31 for QNX 8.0 (see capability matrix)
Version: 3.16.2
Libs: -L\${libdir} -lfuse3
Cflags: -I\${includedir}/fuse3 -D_FILE_OFFSET_BITS=64
PC
cat > "$prefix/lib/pkgconfig/qfuse-dirent.pc" <<PC
prefix=$prefix
includedir=\${prefix}/include

Name: qfuse-dirent
Description: Opt-in QNX directory stream adapter for d_type consumers
Version: 1.0
Requires: fuse3
Cflags: -I\${includedir}/qfuse/compat
PC
printf 'QFuse install prefix: %s\n' "$prefix"

python3 - "$prefix" <<'PYTHON'
from pathlib import Path
import hashlib,json,sys
p=Path(sys.argv[1]); files=[p/'lib/libfuse3.a',p/'lib/pkgconfig/fuse3.pc',p/'lib/pkgconfig/qfuse-dirent.pc',p/'bin/qfuse-unmount',p/'bin/qfuse-mount',*list((p/'include/fuse3').glob('*.h')),p/'include/qfuse/control.h',p/'include/qfuse/admin.h',p/'include/qfuse/compat/dirent.h',*list((p/'share/licenses/qfuse').iterdir())]
if (p/'bin/qfuse-sshfs').is_file():files.append(p/'bin/qfuse-sshfs')
manifest={str(f.relative_to(p)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files}
(p/'share/qfuse').mkdir(parents=True,exist_ok=True)
(p/'share/qfuse/install-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
PYTHON
