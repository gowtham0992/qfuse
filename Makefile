.PHONY: all fetch fetch-libfuse fetch-sshfs check fuse3 sshfs install install-sshfs uninstall source bindfs
all: fuse3
fetch: fetch-libfuse fetch-sshfs
fetch-libfuse:
	python3 scripts/fetch-upstream.py
fetch-sshfs:
	python3 scripts/fetch-sshfs.py
check: fetch-libfuse
	python3 scripts/test-host.py
	python3 -m unittest discover -s tests -p 'test_compiler_wrapper.py'
	python3 -m unittest discover -s tests -p 'test_native_make.py'
fuse3: fetch-libfuse
	bash scripts/build-linux.sh --mount
sshfs: fetch
	bash scripts/build-sshfs-deps.sh
	bash scripts/build-linux.sh --sshfs
install: fuse3
	bash scripts/package-qnx.sh
install-sshfs: sshfs
	bash scripts/package-qnx.sh
uninstall:
	bash -c 'source scripts/target-config.sh && python3 scripts/uninstall-qnx.py "$$QFUSE_INSTALL_PREFIX"'
source:
	python3 scripts/source-dist.py

bindfs: install
	bash scripts/build-bindfs.sh
