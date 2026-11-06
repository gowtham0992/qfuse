# Build and deploy

Use your own activated QNX SDP 8.0 on a Linux SDK host. Obtain it from
[QNX Everywhere](https://qnx.software/en/developers/get-started/qnx-everywhere).
The repository never supplies SDK headers, runtime, images or licenses.

The build recipes have run in Ubuntu 22.04 and 24.04 userlands. The local Mac
arrangement runs Linux SDK tools under x86 emulation; QNX does not support a
native macOS/ARM SDK host. Native Linux SDK users can use the same scripts without
Colima. See the optional container lab below.

## Compile

Install host tools: Bash, GNU make, gcc/g++, Python 3/venv/pip, patch, pkg-config,
ninja, tar and xz. The scripts create user-owned build/dependency directories.

```sh
make check
export QNX_SDP_ROOT=/opt/qnx800
source "$QNX_SDP_ROOT/qnxsdp-env.sh"
export QFUSE_ARCH=x86_64     # aarch64le selects the ARM target
make install-sshfs
source scripts/target-config.sh
printf 'Install prefix: %s\n' "$QFUSE_INSTALL_PREFIX"
```

`make install` builds the library, headers, pkg-config metadata, unmount utility
and background launcher. `make install-sshfs` additionally builds SSHFS and its
real GLib/libffi dependencies. Downloads are checked against lock files. SDK
compiler ABI probes are not replaced with assumed feature answers.

Native Linux defaults use the user's cache directory. Existing development
container paths are preserved for compatibility. Override `QFUSE_WORK_ROOT`,
`QFUSE_BUILD_PARENT`, `QFUSE_DEPS_ROOT`, `QFUSE_TOOLS_ROOT` and
`QFUSE_INSTALL_PREFIX` as needed. Keep build/install paths outside the SDK and
use a Linux-native filesystem for emulated compilation.

Installed-package verification:

```sh
project_root=$PWD
source scripts/qnx-compiler-env.sh
python3 scripts/verify-package.py
```

## Own target

The target needs a matching QNX 8.0 runtime and OpenSSH client. Deploy the QNX
applications and their dependency closure into a private target prefix. The
optional deployment helper stages the owner's SDK libraries privately; those
libraries must never enter a public repository or release archive.

Configure `QFUSE_TARGET_HOST`, `QFUSE_TARGET_PORT`, `QFUSE_TARGET_DIR` and target
`QFUSE_TARGET_IDENTITY` and `QFUSE_TARGET_KNOWN_HOSTS`, or use your existing deployment tools. Library
artifacts and pkg-config metadata stay on the SDK host; executable tools and
runtime dependencies run on QNX. The [usage guide](usage.md) covers the native
commands.

## Optional Docker/QEMU lab

The included Linux development image contains host tools, with the owner's SDK
installed in a private volume. It contains no SDK in its image layers. The local
Colima setup is one optional arrangement. On a Linux Docker host:

```sh
docker build -f Dockerfile.dev -t qfuse-dev:local .
docker volume create qfuse-home
docker run -d --name qfuse-dev --platform linux/amd64 \
  --mount type=bind,src="$PWD",dst=/workspace \
  --mount type=volume,src=qfuse-home,dst=/home/qnx \
  qfuse-dev:local
```

Install QNX Software Center and activate your own SDP in that private volume,
following QNX's account instructions. For an existing Software Center install,
`scripts/install-sdk.py` is interactive and keeps credentials out of the checkout.
Run builds in the container as its `qnx` user. No published container includes
the SDK, a license, or a prebuilt target image.

`Dockerfile.target` runs target emulation and can be built natively for an ARM
Linux Docker host. The SDK build container remains separate. QEMU targets and
private keys live in the private home volume.

For ARM QEMU, install the official QEMU startup and VirtIO block-driver packages
through QNX Software Center. `scripts/install-qemu-startup.py` provides an
interactive account-authenticated helper; credentials stay outside the checkout.
The image generator alone does not validate every optional driver.

```sh
# Inside the SDK environment/container, while this target is stopped:
QFUSE_ARCH=aarch64le QFUSE_TEST_MOUNT=1 QFUSE_TEST_PASSTHROUGH=0 bash scripts/build-qnx-smoke.sh
```

Run the matching architecture with `scripts/run-qnx-smoke.sh`. The lab binds
control SSH to container loopback, uses port 2222 for x86 and 2224 for ARM by
default, and publishes no Mac host ports. Optional SFTP forwarding uses the
isolated loopback fixture. Stop a guest before rebuilding its image.
