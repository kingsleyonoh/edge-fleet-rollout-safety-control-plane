#!/usr/bin/env bash
set -euo pipefail
manifest=${1:?usage: benchmark-report.sh MANIFEST [OUTPUT_DIR]}
output_dir=${2:-./data/benchmark-reports}
binary=./build/dev/edgefleet
[[ -x "$binary" ]] || binary=./build/production/edgefleet
[[ -x "$binary" ]] || { echo "build edgefleet before generating a report" >&2; exit 1; }
mkdir -p "$output_dir"
export EDGEFLEET_LOG_LEVEL=error
"$binary" benchmark --corpus "$manifest" > "$output_dir/report.json"
grep -q '"cell_count":108' "$output_dir/report.json" || { echo "benchmark did not produce 108 cells" >&2; exit 1; }
echo "{\"status\":\"completed\",\"output\":\"$(cd "$output_dir" && pwd)\"}"
