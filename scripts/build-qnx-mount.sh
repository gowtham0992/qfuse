#!/usr/bin/env bash
set -eo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$project_root/scripts/target-config.sh"
sdk_root=${QNX_SDP_ROOT:-/home/qnx/qnx800}
source "$sdk_root/qnxsdp-env.sh" >/dev/null
cd "$project_root"
source scripts/qnx-compiler-env.sh
# Generate and verify the locked fixture dependencies using the existing build.
python3 scripts/test-host.py --qnx
cp .cache/libfuse/example/passthrough.c build/qnx/passthrough.c
cmp .cache/libfuse/example/passthrough.c build/qnx/passthrough.c
qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 \
    -Wall -Wextra -Werror -Iinclude -I.cache/libfuse/include -Ibuild/qnx \
    -c src/qnx_backend.c -o build/qnx/qnx_backend.o
"$QFUSE_BIN_PREFIX-ar" rcs build/qnx/libfuse3.a build/qnx/core.o build/qnx/qnx_backend.o build/qnx/fuse_opt.o build/qnx/fuse_log.o
# hello.c keeps its original main here, unlike the in-process callback test.
qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 \
    -I.cache/libfuse/include -Ibuild/qnx -c .cache/libfuse/example/hello.c -o build/qnx/hello-mount.o
qcc -V"$QFUSE_CC_VARIANT" build/qnx/hello-mount.o -Lbuild/qnx -lfuse3 \
    -o build/qnx/qfuse-hello
qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 \
    -g -O1 -Wall -Wextra -Werror tests/test_mount.c -o build/qnx/test-mount
qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 \
    -Wall -Wextra -Werror -I.cache/libfuse/include -Ibuild/qnx \
    tests/context_fs.c -Lbuild/qnx -lfuse3 -o build/qnx/qfuse-context
qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 -DHAVE_UTIMENSAT \
    -Wall -Wextra -Iinclude/qfuse/compat -I.cache/libfuse/include -I.cache/libfuse/example -Ibuild/qnx \
    build/qnx/passthrough.c -Lbuild/qnx -lfuse3 -o build/qnx/qfuse-passthrough
qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 \
    -Wall -Wextra -Werror tests/test_passthrough.c -o build/qnx/test-passthrough

qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 -Wall -Wextra -Werror tests/test_conformance.c -o build/qnx/test-conformance

qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -Wall -Wextra -Werror -Iinclude src/unmount.c -o build/qnx/qfuse-unmount

qcc -V"$QFUSE_CC_VARIANT" -std=c11 tests/exec_probe.c -o build/qnx/exec-probe

qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 -Wall -Wextra -Werror tests/test_lifecycle.c -o build/qnx/test-lifecycle

qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 -Wall -Wextra -Werror tests/test_write_permissions.c -o build/qnx/test-write-permissions

qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 -Wall -Wextra -Werror tests/test_concurrency.c -o build/qnx/test-concurrency

qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 -Wall -Wextra -Werror -I.cache/libfuse/include -Ibuild/qnx tests/priority_fs.c -Lbuild/qnx -lfuse3 -o build/qnx/qfuse-priority
qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 -Wall -Wextra -Werror tests/test_priority.c -o build/qnx/test-priority

qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 -Wall -Wextra -Werror -I.cache/libfuse/include -Ibuild/qnx tests/test_abi.c -Lbuild/qnx -lfuse3 -o build/qnx/test-abi

qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 -Wall -Wextra -Werror tests/test_mount_identity.c -o build/qnx/test-mount-identity

# Test-only interception of the byte-identical upstream source.
qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -DHAVE_UTIMENSAT \
    -Dmain=qf_upstream_passthrough_main -Dfuse_main_real=qf_inflight_main \
    -Iinclude/qfuse/compat -I.cache/libfuse/include -I.cache/libfuse/example -Ibuild/qnx \
    -c build/qnx/passthrough.c -o build/qnx/inflight-passthrough.o
qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 \
    -Wall -Wextra -Werror -Itests -I.cache/libfuse/include -Ibuild/qnx \
    tests/inflight_passthrough.c build/qnx/inflight-passthrough.o -Lbuild/qnx -lfuse3 -o build/qnx/qfuse-inflight
qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 \
    -Wall -Wextra -Werror -Itests tests/test_inflight_death.c -o build/qnx/test-inflight-death

qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 -Wall -Wextra -Werror tests/test_operation_report.c -o build/qnx/test-operation-report

qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 -Wall -Wextra -Werror -Iinclude/qfuse/compat tests/test_dirent_adapter.c -o build/qnx/test-dirent-adapter
qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 -Wall -Wextra -Werror tests/test_passthrough_plus.c -o build/qnx/test-passthrough-plus

qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 -Wall -Wextra -Werror -Itests tests/test_stall.c -o build/qnx/test-stall

qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 -Wall -Wextra -Werror -Itests tests/test_stall_load.c -o build/qnx/test-stall-load

qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 -Wall -Wextra -Werror -Iinclude src/mount.c -o build/qnx/qfuse-mount
qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 -Wall -Wextra -Werror -Iinclude tests/test_admin.c -o build/qnx/test-admin

qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 \
    -Wall -Wextra -Werror -Itests tests/test_namespace.c -o build/qnx/test-namespace

qcc -V"$QFUSE_CC_VARIANT" -std=c11 -D_QNX_SOURCE -D_FILE_OFFSET_BITS=64 -g -O1 \
    -Wall -Wextra -Werror tests/test_service_permissions.c -o build/qnx/test-service-permissions
