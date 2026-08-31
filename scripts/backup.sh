#!/usr/bin/env bash
set -euo pipefail
destination="${1:?backup destination required}"
mkdir -p "$destination"
binary="${EDGEFLEET_BIN:-edgefleet}"
export EDGEFLEET_LOG_LEVEL=error
"$binary" backup --out "$destination/edgefleet.db"
for name in artifacts traces exports; do
  case "$name" in
    artifacts) path="${ARTIFACT_STORE_PATH:-./data/artifacts}" ;;
    traces) path="${TRACE_STORE_PATH:-./data/traces}" ;;
    exports) path="${EXPORT_STORE_PATH:-./data/exports}" ;;
  esac
  if [[ -d "$path" ]]; then tar -C "$path" -czf "$destination/$name.tgz" .; fi
done
"$binary" evidence --verify > "$destination/evidence-verification.json"
