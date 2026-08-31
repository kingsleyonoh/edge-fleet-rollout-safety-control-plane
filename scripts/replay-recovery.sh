#!/usr/bin/env bash
set -euo pipefail
binary="${EDGEFLEET_BIN:-edgefleet}"
export EDGEFLEET_LOG_LEVEL=error
"$binary" replay-recovery
