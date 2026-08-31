#!/usr/bin/env bash
set -euo pipefail
source_dir="${1:?backup directory required}"
binary="${EDGEFLEET_BIN:-edgefleet}"
export EDGEFLEET_LOG_LEVEL=error
mkdir -p "$(dirname "${SQLITE_PATH:-./data/edgefleet.db}")" "${ARTIFACT_STORE_PATH:-./data/artifacts}" "${TRACE_STORE_PATH:-./data/traces}" "${EXPORT_STORE_PATH:-./data/exports}"
cp "$source_dir/edgefleet.db" "${SQLITE_PATH:-./data/edgefleet.db}"
for name in artifacts traces exports; do
  case "$name" in
    artifacts) path="${ARTIFACT_STORE_PATH:-./data/artifacts}" ;;
    traces) path="${TRACE_STORE_PATH:-./data/traces}" ;;
    exports) path="${EXPORT_STORE_PATH:-./data/exports}" ;;
  esac
  archive="$source_dir/$name.tgz"
  if [[ -f "$archive" ]]; then tar -C "$path" -xzf "$archive"; fi
done
"$binary" evidence --verify
