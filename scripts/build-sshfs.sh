#!/usr/bin/env bash
# Run from a Linux-native source snapshot after real GLib is installed.
set -eo pipefail
export QEMU_CPU=${QFUSE_BUILD_CPU:-Nehalem}
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$project_root/scripts/target-config.sh"
source "$QNX_SDP_ROOT/qnxsdp-env.sh" >/dev/null
cd "$project_root"
source scripts/qnx-compiler-env.sh
sysroot=$QFUSE_DEPS_ROOT/sysroot
test -f "$sysroot/usr/lib/pkgconfig/glib-2.0.pc"
python3 - <<'PY'
import hashlib,json
from pathlib import Path
lock=json.loads(Path('sshfs-dependencies.lock.json').read_text())
for name,item in lock['files'].items():
 if not name.startswith('sshfs/'): continue
 path=Path('.cache')/name
 if hashlib.sha256(path.read_bytes()).hexdigest()!=item['sha256']: raise SystemExit('Changed SSHFS source: '+name)
PY
bash scripts/build-qnx-mount.sh
mkdir -p build/qnx/sshfs
cp .cache/sshfs/sshfs.c build/qnx/sshfs/sshfs.c
patch --silent build/qnx/sshfs/sshfs.c < patches/sshfs-qnx.patch
printf '#define PACKAGE_VERSION "3.7.3"\n#define IDMAP_DEFAULT "none"\n' > build/qnx/sshfs/config.h
export PKG_CONFIG_LIBDIR=$sysroot/usr/lib/pkgconfig:$sysroot/usr/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$sysroot
read -r -a glib_flags <<< "$(pkg-config --cflags glib-2.0 gthread-2.0)"
read -r -a glib_libraries <<< "$(pkg-config --libs glib-2.0 gthread-2.0)"
qcc -V"$QFUSE_CC_VARIANT" -std=gnu11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=31 -DHAVE_CONFIG_H \
    -g -O1 -Wall -Wextra -Wno-sign-compare -I.cache/libfuse/include -Ibuild/qnx \
    -Ibuild/qnx/sshfs -I.cache/sshfs -I"$sysroot/usr/include" "${glib_flags[@]}" \
    build/qnx/sshfs/sshfs.c .cache/sshfs/cache.c -Lbuild/qnx -lfuse3 -L"$sysroot/usr/lib" "${glib_libraries[@]}" \
    -Wl,-rpath-link,"$sysroot/usr/lib" -Wl,-rpath-link,"$QNX_TARGET/$QFUSE_ARCH/usr/lib" \
    -lsocket -lm -o build/qnx/qfuse-sshfs
qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 \
    -Wall -Wextra -Werror tests/test_sshfs.c -o build/qnx/test-sshfs

qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 -Wall -Wextra -Werror tests/test_sshfs_write.c -o build/qnx/test-sshfs-write
qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 \
    -I"$sysroot/usr/include" $(pkg-config --cflags glib-2.0 libffi) tests/test_dependencies.c \
    -L"$sysroot/usr/lib" $(pkg-config --libs glib-2.0 libffi) -lm -lsocket -o build/qnx/test-dependencies
