#!/usr/bin/env bash
# Optional second third-party filesystem; use real SDK configure probes.
set -euo pipefail
export QEMU_CPU=${QFUSE_BUILD_CPU:-Nehalem}
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$project_root/scripts/target-config.sh"
source "$QNX_SDP_ROOT/qnxsdp-env.sh" >/dev/null
python3 "$project_root/scripts/fetch-bindfs.py"
# Install a matching freshly built library/package before configuring bindfs.
bash "$project_root/scripts/package-qnx.sh"
mkdir -p "$QFUSE_BUILD_PARENT"
port_root=$(mktemp -d "$QFUSE_BUILD_PARENT/bindfs.XXXXXX")
python3 - "$project_root" "$port_root" <<'PY'
from pathlib import Path
import hashlib,json,sys,tarfile
root=Path(sys.argv[1]);destination=Path(sys.argv[2]);lock=json.loads((root/'bindfs.lock.json').read_text())
archive=root/'.cache'/('bindfs-'+lock['version']+'.tar.gz')
assert hashlib.sha256(archive.read_bytes()).hexdigest()==lock['sha256']
with tarfile.open(archive) as source:
 for item in source:
  relative=Path(*Path(item.name).parts[1:])
  if relative.is_absolute() or '..' in relative.parts:raise SystemExit('Unsafe upstream path')
  target=destination/relative
  if item.isdir():target.mkdir(parents=True,exist_ok=True)
  elif item.isfile():
   target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(source.extractfile(item).read());target.chmod(item.mode&0o777)
PY
cd "$port_root"
patch --batch -p1 < "$project_root/patches/bindfs-qnx.patch"
export QFUSE_COMPILER_FAILURE_FILE="$port_root/compiler-failure"
export CC="python3 $project_root/scripts/qnx-compiler.py $QNX_HOST/usr/bin/qcc -V$QFUSE_CC_VARIANT"
export AR=$QFUSE_BIN_PREFIX-ar RANLIB=$QFUSE_BIN_PREFIX-ranlib
export PKG_CONFIG_LIBDIR=$QFUSE_INSTALL_PREFIX/lib/pkgconfig
unset PKG_CONFIG_SYSROOT_DIR
libtoolize --force --copy
autoreconf -fi
cpu=$QFUSE_ARCH
[[ $cpu != aarch64le ]] || cpu=aarch64
./configure --host="$cpu-unknown-nto-qnx8.0.0" --with-fuse3 --prefix=/usr/local CPPFLAGS="-D_QNX_SOURCE -I$QFUSE_INSTALL_PREFIX/include/qfuse/compat"
[[ ! -e $QFUSE_COMPILER_FAILURE_FILE ]]
python3 "$project_root/scripts/qnx-compiler.py" make -j2
[[ ! -e $QFUSE_COMPILER_FAILURE_FILE ]]
python3 "$project_root/scripts/qnx-compiler.py" "$QNX_HOST/usr/bin/qcc" -V"$QFUSE_CC_VARIANT" \
    -D_QNX_SOURCE -Wall -Wextra -Werror "$project_root/tests/test_bindfs.c" -o "$port_root/test-bindfs"
ln -sfn "$port_root" "$QFUSE_BUILD_PARENT/bindfs-latest"
printf 'Built bindfs: %s/src/bindfs\n' "$port_root"
