#!/usr/bin/env bash
set -euo pipefail

destination="${1:?usage: recovery-drill.sh DESTINATION}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_sqlite="${SQLITE_PATH:-$repo_root/data/edgefleet.db}"
source_artifacts="${ARTIFACT_STORE_PATH:-$repo_root/data/artifacts}"
source_traces="${TRACE_STORE_PATH:-$repo_root/data/traces}"
source_exports="${EXPORT_STORE_PATH:-$repo_root/data/exports}"
backup_root="$destination/backup"
restore_root="$destination/restored"
mkdir -p "$backup_root" "$restore_root"

binary="$repo_root/build/dev/edgefleet"
[[ -x "$binary" ]] || binary="$repo_root/build/production/edgefleet"
[[ -x "$binary" ]] || { echo 'Build edgefleet before running a recovery drill.' >&2; exit 1; }
export EDGEFLEET_LOG_LEVEL=error

SQLITE_PATH="$source_sqlite" ARTIFACT_STORE_PATH="$source_artifacts" TRACE_STORE_PATH="$source_traces" EXPORT_STORE_PATH="$source_exports" \
  "$repo_root/scripts/backup.sh" "$backup_root"

source_check=$(SQLITE_PATH="$source_sqlite" ARTIFACT_STORE_PATH="$source_artifacts" TRACE_STORE_PATH="$source_traces" EXPORT_STORE_PATH="$source_exports" "$binary" recovery-check --require-completed)
SQLITE_PATH="$restore_root/edgefleet.db" ARTIFACT_STORE_PATH="$restore_root/artifacts" TRACE_STORE_PATH="$restore_root/traces" EXPORT_STORE_PATH="$restore_root/exports" \
  "$repo_root/scripts/restore.sh" "$backup_root"
replay_recovery=$(SQLITE_PATH="$restore_root/edgefleet.db" ARTIFACT_STORE_PATH="$restore_root/artifacts" TRACE_STORE_PATH="$restore_root/traces" EXPORT_STORE_PATH="$restore_root/exports" "$binary" replay-recovery)
restored_check=$(SQLITE_PATH="$restore_root/edgefleet.db" ARTIFACT_STORE_PATH="$restore_root/artifacts" TRACE_STORE_PATH="$restore_root/traces" EXPORT_STORE_PATH="$restore_root/exports" "$binary" recovery-check --require-completed)

python3 - "$source_check" "$restored_check" "$replay_recovery" "$backup_root/edgefleet.db" "$restore_root/edgefleet.db" "$source_artifacts" "$restore_root/artifacts" "$destination/recovery-result.json" <<'PY'
import hashlib, json, pathlib, sys

source, restored, replay = (json.loads(value) for value in sys.argv[1:4])
def tree(path):
    root = pathlib.Path(path)
    return {str(item.relative_to(root)).replace('\\', '/'): hashlib.sha256(item.read_bytes()).hexdigest() for item in sorted(root.rglob('*')) if item.is_file()} if root.exists() else {}
db_match = hashlib.sha256(pathlib.Path(sys.argv[4]).read_bytes()).hexdigest() == hashlib.sha256(pathlib.Path(sys.argv[5]).read_bytes()).hexdigest()
artifacts_match = tree(sys.argv[6]) == tree(sys.argv[7])
source_digests = [item['latest_completed_release']['decision_digest'] for item in source['tenants']]
restored_digests = [item['latest_completed_release']['decision_digest'] for item in restored['tenants']]
decision_match = source_digests == restored_digests
status = 'passed' if db_match and artifacts_match and decision_match and restored.get('status') == 'ok' else 'failed'
result = {'status': status, 'source': source, 'restored': restored, 'database_bytes_match': db_match, 'artifact_bytes_match': artifacts_match, 'latest_completed_decision_match': decision_match, 'replay_recovery': replay}
pathlib.Path(sys.argv[8]).write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps(result, indent=2))
if status != 'passed': raise SystemExit(1)
PY
