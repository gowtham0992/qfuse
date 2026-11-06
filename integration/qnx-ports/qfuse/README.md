# Local QFuse build-files overlay

Copy this directory to `build-files/ports/qfuse`. Keep QFuse as a separate source
checkout and source the owner's QNX SDP 8.0 environment. This overlay builds the
verified x86_64 or aarch64le high-level API 31 library, headers and lifecycle tools. It stages into
a private root; it does not install into or alter the SDK.

```sh
make -C build-files/ports/qfuse CPU=x86_64 QFUSE_SOURCE_DIR=/absolute/qfuse install
make -C build-files/ports/qfuse CPU=x86_64 QFUSE_SOURCE_DIR=/absolute/qfuse verify
```

`INSTALL_ROOT_nto` defaults to this recipe's `stage/$(CPU)` directory and `PREFIX` to
`/usr/local`. `QFUSE_BUILD_PARENT` defaults to a separate `build/$(CPU)` directory in the
recipe. Set `QNX_SDP_ROOT` if it cannot be derived from sourced `QNX_HOST`.
Set `CPU=aarch64le` for ARM. `QNX_PROJECT_ROOT` is accepted as the source
checkout variable used by build-files. Unsupported CPUs fail explicitly. Use a Linux-native filesystem for the build
root when compiling under emulation.

The resulting package's absolute pkg-config prefix points to its staging root
for cross-compilation. Regenerate the package when selecting a different prefix;
it is not a prebuilt relocatable target-runtime distribution. The manifest
uninstaller preserves modified and unrelated files.

SSHFS and target tests remain in QFuse's documented dependency/test recipes. This
library overlay intentionally does not vendor the SDK, SSHFS dependencies or
licensed target runtime. Source: https://github.com/gowtham0992/qfuse . Maintainer review and acceptance
are separate from successful local builds; this recipe carries no endorsement.

This recipe and its included license are original QFuse contribution files under MIT.
