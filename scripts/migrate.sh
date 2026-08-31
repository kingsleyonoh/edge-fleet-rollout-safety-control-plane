#!/usr/bin/env bash
set -euo pipefail

binary="./build/dev/edgefleet"
if [[ ! -x "$binary" ]]; then binary="edgefleet"; fi
"$binary" migrate
