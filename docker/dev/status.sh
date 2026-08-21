#!/bin/sh

build_dir=${LOCAL_BUILD_DIR:-/app/build}
cache_file="$build_dir/CMakeCache.txt"
worker="$build_dir/playbacq_worker"

if [ ! -f "$cache_file" ]; then
    echo "Local build configuration: unavailable ($cache_file was not found)"
    exit 1
fi

build_local_dev=$(sed -n 's/^BUILD_LOCAL_DEV:BOOL=//p' "$cache_file" | tail -n 1)
use_local_worker=$(sed -n 's/^USE_LOCAL_WORKER:BOOL=//p' "$cache_file" | tail -n 1)
use_local_gpu=$(sed -n 's/^USE_LOCAL_GPU_ENCODER:BOOL=//p' "$cache_file" | tail -n 1)

case "$build_local_dev:$use_local_gpu" in
    ON:ON)
        mode="LOCAL GPU"
        ;;
    ON:*)
        mode="LOCAL CPU"
        ;;
    *)
        mode="NON-LOCAL"
        ;;
esac

printf '\nLocal build configuration:\n'
printf '  Mode: %s\n' "$mode"
printf '  BUILD_LOCAL_DEV=%s\n' "${build_local_dev:-unknown}"
printf '  USE_LOCAL_WORKER=%s\n' "${use_local_worker:-unknown}"
printf '  USE_LOCAL_GPU_ENCODER=%s\n' "${use_local_gpu:-unknown}"

printf '\nWorker encoder:\n'
if [ -x "$worker" ]; then
    "$worker" --show-encoder-config
else
    echo "  unavailable ($worker was not found)"
fi

printf '\nGPU:\n'
if gpu_info=$(nvidia-smi -L 2>/dev/null); then
    printf '%s\n' "$gpu_info"
else
    echo "  not exposed to the backend container"
fi
