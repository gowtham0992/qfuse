source "$project_root/scripts/target-config.sh"
# Source after qnxsdp-env.sh. All source builds use the tested crash supervisor.
mkdir -p "$project_root/build/qnx"
export QFUSE_COMPILER_FAILURE_FILE="$project_root/build/qnx/compiler-failure"
if [[ -e $QFUSE_COMPILER_FAILURE_FILE ]]; then
    echo 'Compiler infrastructure previously failed; use a fresh build snapshot.' >&2
    return 1
fi
export CC="python3 $project_root/scripts/qnx-compiler.py $QNX_HOST/usr/bin/qcc"
qcc() { python3 "$project_root/scripts/qnx-compiler.py" "$QNX_HOST/usr/bin/qcc" "$@"; }
