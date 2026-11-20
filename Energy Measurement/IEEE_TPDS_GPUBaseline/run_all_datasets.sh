#!/usr/bin/env bash
# Run query options 3 (10%) and 4 (25%) for every available dataset.
set -Eeuo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$project_dir"

if ! command -v nvcc >/dev/null 2>&1; then
    echo "CUDA compiler not found. Load CUDA first (for example: module load cuda-toolkit/12.6)." >&2
    exit 1
fi

cuda_bin_dir="$(dirname -- "$(command -v nvcc)")"
cuda_path="$(cd -- "$cuda_bin_dir/.." && pwd)"
gpu_arch="${GPU_ARCH:-sm_75}"

if [[ ! -x rtree_search || src/host_main.cu -nt rtree_search || src/rtreefunction.c -nt rtree_search ]]; then
    make CUDA_PATH="$cuda_path" ARCH="$gpu_arch"
fi

dataset_names=(Sports Lakes Parks Synthetic Buildings)
dataset_repeat_counts=(150 20 20 20 20)

for dataset in 1 2 3 4 5; do
    repeat_count="${dataset_repeat_counts[dataset - 1]}"
    for query_option in 3 4; do
        printf '\n===== %s: query option %s, repetitions %s =====\n' \
            "${dataset_names[dataset - 1]}" "$query_option" "$repeat_count"
        printf '%s\n%s\n' "$dataset" "$query_option" | REPEAT_COUNT="$repeat_count" ./rtree_search
    done
done
