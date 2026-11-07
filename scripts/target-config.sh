#!/usr/bin/env bash
# Shared target/build defaults. Source this; do not execute it as a command.
export QFUSE_ARCH=${QFUSE_ARCH:-x86_64}
export QFUSE_TARGET_CPUS=${QFUSE_TARGET_CPUS:-4}
[[ $QFUSE_TARGET_CPUS =~ ^[1-9][0-9]?$ && $QFUSE_TARGET_CPUS -le 64 ]] || { echo 'QFUSE_TARGET_CPUS must be an integer from 1 to 64' >&2; return 2; }
case "$QFUSE_ARCH" in
 x86_64) qfuse_bin_prefix=ntox86_64; qfuse_legacy_suffix=; qfuse_port_default=2222; qfuse_legacy_target=x86_64;;
 aarch64le) qfuse_bin_prefix=ntoaarch64; qfuse_legacy_suffix=-aarch64le; qfuse_port_default=2224; qfuse_legacy_target=aarch64;;
 *) echo 'QFUSE_ARCH must be x86_64 or aarch64le' >&2; return 2;;
esac
export QFUSE_CC_VARIANT=gcc_nto${QFUSE_ARCH}
export QFUSE_BIN_PREFIX=$qfuse_bin_prefix
if [[ -z ${QNX_SDP_ROOT:-} ]]; then
 if [[ -n ${QNX_HOST:-} ]]; then QNX_SDP_ROOT=${QNX_HOST%/host/*}; else QNX_SDP_ROOT=$HOME/qnx800; fi
fi
export QNX_SDP_ROOT
# Preserve existing container caches while native Linux users get user-owned
# cache paths. Every directory and SSH endpoint can be overridden explicitly.
if [[ -z ${QFUSE_WORK_ROOT:-} && $HOME == /home/qnx && -d /home/qnx/qfuse-builds ]]; then
 export QFUSE_BUILD_PARENT=${QFUSE_BUILD_PARENT:-/home/qnx/qfuse-builds${qfuse_legacy_suffix}}
 export QFUSE_DEPS_ROOT=${QFUSE_DEPS_ROOT:-/home/qnx/qfuse-deps${qfuse_legacy_suffix}}
 export QFUSE_TOOLS_ROOT=${QFUSE_TOOLS_ROOT:-/home/qnx/qfuse-tools}
 export QFUSE_INSTALL_PREFIX=${QFUSE_INSTALL_PREFIX:-/home/qnx/qfuse-install${qfuse_legacy_suffix}}
 export QFUSE_TARGET_DIR=${QFUSE_TARGET_DIR:-/home/qnx/targets/qfuse-${qfuse_legacy_target}}
else
 qfuse_work_root=${QFUSE_WORK_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/qfuse}
 export QFUSE_BUILD_PARENT=${QFUSE_BUILD_PARENT:-$qfuse_work_root/builds/$QFUSE_ARCH}
 export QFUSE_DEPS_ROOT=${QFUSE_DEPS_ROOT:-$qfuse_work_root/deps/$QFUSE_ARCH}
 export QFUSE_TOOLS_ROOT=${QFUSE_TOOLS_ROOT:-$qfuse_work_root/tools}
 export QFUSE_INSTALL_PREFIX=${QFUSE_INSTALL_PREFIX:-$qfuse_work_root/install/$QFUSE_ARCH}
 export QFUSE_TARGET_DIR=${QFUSE_TARGET_DIR:-$qfuse_work_root/targets/$QFUSE_ARCH}
fi
export QFUSE_ARTIFACT_ROOT=${QFUSE_ARTIFACT_ROOT:-$QFUSE_BUILD_PARENT/latest/build/qnx}
export QFUSE_TARGET_HOST=${QFUSE_TARGET_HOST:-127.0.0.1}
export QFUSE_TARGET_PORT=${QFUSE_TARGET_PORT:-$qfuse_port_default}
[[ $QFUSE_TARGET_PORT =~ ^[0-9]+$ && $QFUSE_TARGET_PORT -ge 1 && $QFUSE_TARGET_PORT -le 65535 ]] || { echo 'Invalid target SSH port' >&2; return 2; }

export QFUSE_TARGET_IDENTITY=${QFUSE_TARGET_IDENTITY:-$QFUSE_TARGET_DIR/client_ed25519}
export QFUSE_TARGET_KNOWN_HOSTS=${QFUSE_TARGET_KNOWN_HOSTS:-$QFUSE_TARGET_DIR/known_hosts}
