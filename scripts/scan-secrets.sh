#!/usr/bin/env bash
set -euo pipefail
mode=all
paths=()
while (($#)); do
  case "$1" in
    --mode) mode="$2"; shift 2;;
    --) shift; paths+=("$@"); break;;
    *) paths+=("$1"); shift;;
  esac
done
case "$mode" in staged) mapfile -t files < <(git diff --cached --name-only --diff-filter=ACM);; tracked) mapfile -t files < <(git ls-files);; all) mapfile -t files < <({ git ls-files; git ls-files --others --exclude-standard; } | sort -u);; paths) files=("${paths[@]}");; *) echo '{"status":"error","error":"invalid mode"}'; exit 2;; esac
if ((${#files[@]} == 0)); then echo '{"status":"error","error":"secret scan selected no files"}'; exit 2; fi
matches=()
for file in "${files[@]:-}"; do
  [[ -f "$file" && ! "$file" =~ \.lock$|\.min\.(js|css)$|[/\\]build[/\\]|[/\\]node_modules[/\\]|[/\\]vendor[/\\]|scan-secrets\.(ps1|sh)$ ]] || continue
  line_no=0
  while IFS= read -r line || [[ -n "$line" ]]; do
    line_no=$((line_no + 1))
    [[ "$line" =~ \$\{[A-Z_][A-Z0-9_]*\}|\<REDACTED\>|placeholder|redacted|your[-_].*here|example|fake|dummy|sample|\.\.\. ]] && continue
    name= pattern=
    if [[ "$line" =~ eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,} ]]; then name=jwt; pattern=jwt
    elif [[ "$line" =~ (sk-(proj-)?|ghp_|gho_|glpat-|xoxb-|AKIA|AIza)[A-Za-z0-9_-]{16,} ]]; then name=provider-key; pattern=provider-key
    elif [[ "$line" =~ postgres(ql)?://[^:[:space:]]+:[^@[:space:]]+@[^/[:space:]]+/[^[:space:]]+ ]]; then name=database-url; pattern=database-url
    elif [[ "$line" =~ [Aa]uthorization:[[:space:]]*[Bb]earer[[:space:]]+[A-Za-z0-9._-]{20,} ]]; then name=bearer; pattern=bearer
    elif [[ "$line" =~ (api[_-]?key|secret|token|password|client[_-]?secret)[[:space:]]*[:=][[:space:]]*[\"'][A-Za-z0-9._+/=-]{30,}[\"'] ]]; then name=secret-assignment; pattern=secret-assignment; fi
    [[ -n "$name" ]] && matches+=("{\"file\":\"${file//\/\\}\",\"line\":$line_no,\"pattern\":\"$pattern\"}")
  done < "$file"
done
if ((${#matches[@]})); then status=secrets_detected; else status=clean; fi
printf '{\n  "matches": ['
if ((${#matches[@]})); then joined=$(IFS=,; echo "${matches[*]}"); printf '%s' "$joined"; fi
printf '],\n  "files_scanned": %d,\n  "mode": "%s",\n  "status": "%s"\n}\n' "${#files[@]}" "$mode" "$status"
if ((${#matches[@]})); then exit 1; fi
