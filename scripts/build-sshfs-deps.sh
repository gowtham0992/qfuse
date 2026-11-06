#!/usr/bin/env bash
# Build real GLib in a private prefix; never alter the licensed SDK.
set -eo pipefail
export QEMU_CPU=${QFUSE_BUILD_CPU:-Nehalem}
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$project_root/scripts/target-config.sh"
source "$QNX_SDP_ROOT/qnxsdp-env.sh" >/dev/null
dependency_root=$QFUSE_DEPS_ROOT
sysroot=$dependency_root/sysroot
cpu_family=$QFUSE_ARCH
if [[ $QFUSE_ARCH == aarch64le ]]; then cpu_family=aarch64; fi
host_triplet=${cpu_family}-unknown-nto-qnx8.0.0
mkdir -p "$dependency_root/src" "$dependency_root/build" "$sysroot/usr/lib/pkgconfig"
export QFUSE_COMPILER_FAILURE_FILE=$dependency_root/build/compiler-emulation-failed
if [[ -e $QFUSE_COMPILER_FAILURE_FILE ]]; then
    echo 'An earlier compiler crash invalidated this build. Inspect and discard its configuration before retrying.' >&2
    exit 1
fi
python3 - "$project_root" <<'PY'
import hashlib,json,sys
from pathlib import Path
root=Path(sys.argv[1]); lock=json.loads((root/'sshfs-dependencies.lock.json').read_text())
for name, item in lock['files'].items():
 path=root/'.cache'/('sources/'+name if not name.startswith('sshfs/') else name)
 if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=item['sha256']:
  raise SystemExit('Missing or changed dependency: '+name)
print('Verified dependency source hashes.')
PY
if [[ ! -x $QFUSE_TOOLS_ROOT/bin/meson ]]; then
    python3 -m venv $QFUSE_TOOLS_ROOT
    $QFUSE_TOOLS_ROOT/bin/pip install meson==1.7.2
fi
if [[ ! -d $dependency_root/src/libffi-3.4.6 ]]; then
    tar -xf "$project_root/.cache/sources/libffi-3.4.6.tar.gz" -C "$dependency_root/src"
fi
if [[ ! -d $dependency_root/src/glib-2.84.3 ]]; then
    tar -xf "$project_root/.cache/sources/glib-2.84.3.tar.xz" -C "$dependency_root/src"
    patch --batch -d "$dependency_root/src/glib-2.84.3" -p1 < "$project_root/.cache/sources/glib-qnx-atomic.patch"
fi
mkdir -p "$dependency_root/build/libffi"
cd "$dependency_root/build/libffi"
# QNX enforces non-executable heap memory. libffi 3.4.6's generic
# allocator uses malloc unless this supported mmap allocator is selected.
# This is a port configuration, not an assumed configure-probe answer.
if [[ -f Makefile && ! -f qfuse-executable-closures ]]; then
    make distclean
fi
if [[ ! -f Makefile ]]; then
    CC="python3 $project_root/scripts/qnx-compiler.py qcc -V$QFUSE_CC_VARIANT" \
    CXX="python3 $project_root/scripts/qnx-compiler.py ${QFUSE_BIN_PREFIX}-g++" CFLAGS='-O1 -fPIC -D_QNX_SOURCE -DFFI_MMAP_EXEC_WRIT=1' \
    AR=${QFUSE_BIN_PREFIX}-ar RANLIB=${QFUSE_BIN_PREFIX}-ranlib \
        "$dependency_root/src/libffi-3.4.6/configure" \
        --host="$host_triplet" --prefix=/usr --libdir=/usr/lib --disable-docs
fi
test ! -e "$QFUSE_COMPILER_FAILURE_FILE"
touch qfuse-executable-closures
python3 "$project_root/scripts/qnx-compiler.py" make -j2
test ! -e "$QFUSE_COMPILER_FAILURE_FILE"
# Retry the complete install transaction on an emulated ranlib crash: libtool
# copies a fresh archive before indexing it, rather than retrying partial data.
python3 "$project_root/scripts/qnx-compiler.py" make DESTDIR="$sysroot" install
test ! -e "$QFUSE_COMPILER_FAILURE_FILE"
# zlib is supplied by the SDK and found by qcc's normal target search paths.
zlib_version=$(sed -n 's/^#define ZLIB_VERSION "\([^"]*\)"/\1/p' "$QNX_TARGET/usr/include/zlib.h")
printf 'Name: zlib\nDescription: QNX SDK zlib\nVersion: %s\nLibs: -lz\n' "$zlib_version" > "$sysroot/usr/lib/pkgconfig/zlib.pc"
cat > "$dependency_root/qnx-cross.ini" <<EOF
[binaries]
c = ['python3', '$project_root/scripts/qnx-compiler.py', '${QFUSE_BIN_PREFIX}-gcc']
cpp = ['python3', '$project_root/scripts/qnx-compiler.py', '${QFUSE_BIN_PREFIX}-g++']
ar = '${QFUSE_BIN_PREFIX}-ar'
strip = '${QFUSE_BIN_PREFIX}-strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'qnx'
cpu_family = '$cpu_family'
cpu = '$cpu_family'
endian = 'little'
[properties]
needs_exe_wrapper = true
growing_stack = false
sys_root = '$sysroot'
pkg_config_libdir = ['$sysroot/usr/lib/pkgconfig', '$sysroot/usr/share/pkgconfig']
[built-in options]
c_args = ['-D_QNX_SOURCE', '-I$sysroot/usr/include']
cpp_args = c_args
c_link_args = ['-L$sysroot/usr/lib', '-Wl,-rpath-link,$sysroot/usr/lib', '-lm', '-lsocket', '-lgcc_s']
cpp_link_args = c_link_args
EOF
if [[ ! -f $dependency_root/build/glib/build.ninja ]]; then
    $QFUSE_TOOLS_ROOT/bin/meson setup "$dependency_root/build/glib" "$dependency_root/src/glib-2.84.3" \
        --cross-file "$dependency_root/qnx-cross.ini" --prefix=/usr --libdir=lib \
        --buildtype=release -Dtests=false -Dxattr=false -Dnls=disabled \
        -Dselinux=disabled -Dlibmount=disabled -Dintrospection=disabled \
        -Ddocumentation=false -Dman-pages=disabled -Dlibelf=disabled -Dsysprof=disabled
fi
test ! -e "$QFUSE_COMPILER_FAILURE_FILE"
$QFUSE_TOOLS_ROOT/bin/meson configure "$dependency_root/build/glib" \
    "-Dc_args=['-D_QNX_SOURCE', '-I$sysroot/usr/include']" \
    "-Dcpp_args=['-D_QNX_SOURCE', '-I$sysroot/usr/include']" \
    "-Dc_link_args=['-L$sysroot/usr/lib', '-Wl,-rpath-link,$sysroot/usr/lib', '-lm', '-lsocket', '-lgcc_s']" \
    "-Dcpp_link_args=['-L$sysroot/usr/lib', '-Wl,-rpath-link,$sysroot/usr/lib', '-lm', '-lsocket', '-lgcc_s']"
ninja -C "$dependency_root/build/glib" -j2
DESTDIR="$sysroot" $QFUSE_TOOLS_ROOT/bin/meson install -C "$dependency_root/build/glib" --no-rebuild
printf 'GLib dependency prefix: %s\n' "$sysroot/usr"
