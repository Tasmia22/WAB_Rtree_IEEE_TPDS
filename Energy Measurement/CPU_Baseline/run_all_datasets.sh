#!/usr/bin/env bash
# Run every CPU-baseline dataset using query options 3 (10%) and 4 (25%).
set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "$script_dir/.." && pwd)"
binary="$script_dir/rtree_cpu_baseline"
log_dir="$script_dir/run_logs"
combined_log="$script_dir/CPU_Baseline.log"

cd "$project_dir"

# The current linker cannot parse the macOS 27 SDK installed on this machine.
# Use the locally available, compatible SDK unless the caller selected one.
if [[ -z "${SDKROOT:-}" && -d /Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk ]]; then
    export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk
    export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-15.0}"
fi

# Rebuild when needed.  The program uses paths relative to the project root.
if [[ ! -x "$binary" || "$script_dir/rtree.c" -nt "$binary" || \
      "$script_dir/rtreefunction.c" -nt "$binary" || "$script_dir/rtree.h" -nt "$binary" ]]; then
    make -C "$script_dir" rtree_cpu_baseline
fi

mkdir -p "$log_dir"
: > "$combined_log"

dataset_names=(Sports Lakes Parks Synthetic Buildings)
dataset_repeat_counts=(150 20 20 20 20)
query_labels=(unused '1pct' '5pct' '10pct' '25pct')

if (( $# > 0 )); then
    datasets=("$@")
else
    datasets=(1 2 3 4 5)
fi

for dataset in "${datasets[@]}"; do
    repeat_count="${dataset_repeat_counts[dataset - 1]}"
    for query_option in 3 4; do
        dataset_name="${dataset_names[dataset - 1]}"
        log_file="$log_dir/dataset_${dataset}_query_${query_labels[query_option]}.log"

        printf '\n===== %s: query option %s (%s), repetitions %s =====\n' \
            "$dataset_name" "$query_option" "${query_labels[query_option]}" "$repeat_count" | tee -a "$combined_log"
        printf '%s\n%s\n' "$dataset" "$query_option" | REPEAT_COUNT="$repeat_count" "$binary" | tee "$log_file" | tee -a "$combined_log"
    done
done

printf '\nCombined log saved to: %s\n' "$combined_log"
