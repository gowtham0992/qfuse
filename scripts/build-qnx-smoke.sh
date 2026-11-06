#!/usr/bin/env bash
# Build inside qfuse-dev. Proprietary images remain in the private home volume.
set -eo pipefail
export QEMU_CPU=${QFUSE_BUILD_CPU:-Nehalem}
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$project_root/scripts/target-config.sh"
sdk_root=$QNX_SDP_ROOT
target_root=$QFUSE_TARGET_DIR
source "$sdk_root/qnxsdp-env.sh" >/dev/null
cd "$project_root"
if [[ ${QFUSE_TEST_MOUNT:-0} == 1 ]]; then
    bash scripts/build-linux.sh --mount
else
    bash scripts/build-linux.sh --callbacks
fi
artifact_root=$QFUSE_ARTIFACT_ROOT
mkdir -p "$target_root"
if [[ ! -f "$target_root/client_ed25519" ]]; then
    ssh-keygen -q -t ed25519 -N '' -f "$target_root/client_ed25519"
fi
cd "$target_root"
if [[ ! -f local/options ]]; then
    mkqnximage --type=qemu --arch="$QFUSE_ARCH" --hostname=qfuse-target --cpu=1 --ram=512M
fi
mkdir -p local/snippets
printf '[perms=0755] /usr/bin/qfuse-test-core=%s/test-core\n' "$artifact_root" > local/snippets/ifs_files.qfuse
if [[ ${QFUSE_TEST_MOUNT:-0} == 1 ]]; then
    printf '[perms=0755] /usr/bin/qfuse-hello=%s/qfuse-hello\n[perms=0755] /usr/bin/test-mount=%s/test-mount\n' \
        "$artifact_root" "$artifact_root" >> local/snippets/ifs_files.qfuse
    printf '[perms=0755] /usr/bin/qfuse-context=%s/qfuse-context\n' \
        "$artifact_root" >> local/snippets/ifs_files.qfuse
    printf '[perms=0755] /usr/bin/qfuse-passthrough=%s/qfuse-passthrough\n[perms=0755] /usr/bin/test-passthrough=%s/test-passthrough\n' \
        "$artifact_root" "$artifact_root" >> local/snippets/ifs_files.qfuse
fi
# Current OpenSSH packages split these helpers out from sshd. Include them
# in the IFS namespace used by sshd, rather than only under /system.
for helper in sshd-auth sshd-session; do
    test -f "$QNX_TARGET/$QFUSE_ARCH/usr/libexec/$helper"
done
printf '[perms=0755] /usr/libexec/sshd-auth=%s/%s/usr/libexec/sshd-auth\n[perms=0755] /usr/libexec/sshd-session=%s/%s/usr/libexec/sshd-session\n' \
    "$QNX_TARGET" "$QFUSE_ARCH" "$QNX_TARGET" "$QFUSE_ARCH" >> local/snippets/ifs_files.qfuse
# Clear the earlier smoke recipe's system-only placement.
: > local/snippets/system_files.qfuse
cat > local/snippets/post_start.qfuse <<'EOF'
echo QFUSE_TARGET_TEST_BEGIN > /dev/ser1
/usr/bin/qfuse-test-core > /dev/ser1 2>&1
qfuse_result=$?
echo QFUSE_TARGET_TEST_EXIT=$qfuse_result > /dev/ser1
EOF
if [[ ${QFUSE_TEST_MOUNT:-0} == 1 ]]; then
    cat >> local/snippets/post_start.qfuse <<'EOF'
echo QFUSE_MOUNT_TEST_BEGIN > /dev/ser1
/usr/bin/test-mount > /dev/ser1 2>&1
qfuse_result=$?
echo QFUSE_MOUNT_TEST_EXIT=$qfuse_result > /dev/ser1
EOF
    if [[ ${QFUSE_TEST_LIFECYCLE:-0} == 1 ]]; then
        cat >> local/snippets/post_start.qfuse <<'EOF'
echo QFUSE_LIFECYCLE_TEST_BEGIN > /dev/ser1
QFUSE_CONTEXT_LIFECYCLE=1 /usr/bin/test-mount --security-only > /dev/ser1 2>&1
qfuse_result=$?
echo QFUSE_LIFECYCLE_TEST_EXIT=$qfuse_result > /dev/ser1
EOF
    fi
    if [[ ${QFUSE_TEST_PASSTHROUGH:-1} == 1 ]]; then
        cat >> local/snippets/post_start.qfuse <<'EOF'
echo QFUSE_PASSTHROUGH_TEST_BEGIN > /dev/ser1
/usr/bin/test-passthrough > /dev/ser1 2>&1
qfuse_result=$?
echo QFUSE_PASSTHROUGH_TEST_EXIT=$qfuse_result > /dev/ser1
EOF
    fi
fi
mkqnximage --ssh-ident="$target_root/client_ed25519.pub" --data-size=128 --data-inodes=24000 --build
