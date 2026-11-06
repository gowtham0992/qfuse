#!/usr/bin/env bash
# SSH binds only the container loopback; no macOS ports are published.
# No shared host filesystems or outbound internet access for the QNX guest.
set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/target-config.sh"
target_root=${QFUSE_TARGET_DIR:-/home/qnx/targets/qfuse-x86_64}
cd "$target_root"
network="user,id=net0,restrict=on,hostfwd=tcp:127.0.0.1:${QFUSE_TARGET_PORT}-:22"
if [[ ${QFUSE_SFTP_TEST:-0} == 1 ]]; then
    network+=',guestfwd=tcp:10.0.2.100:2223-cmd:/usr/bin/nc 127.0.0.1 2223'
fi
common=(-pidfile output/qfuse-smoke.pid -accel tcg -smp 1 -m 512M
        -kernel output/ifs.bin -display none -serial stdio -monitor none)
if [[ $QFUSE_ARCH == aarch64le ]]; then
    exec qemu-system-aarch64 "${common[@]}" -machine virt-4.2 -cpu cortex-a57 \
        -drive file=output/disk-qemu.vmdk,format=vmdk,if=none,id=drv0 \
        -device virtio-blk-device,drive=drv0 \
        -netdev "$network" -device virtio-net-device,netdev=net0 \
        -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-device,rng=rng0
else
    exec qemu-system-x86_64 "${common[@]}" -cpu max \
        -drive file=output/disk-qemu.vmdk,format=vmdk,if=ide,id=drv0 \
        -netdev "$network" -device virtio-net-pci,netdev=net0 \
        -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0
fi
