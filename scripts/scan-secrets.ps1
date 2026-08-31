[CmdletBinding()]
param(
    [ValidateSet("staged", "tracked", "all", "paths")]
    [string]$Mode = "all",
    [string[]]$Paths = @()
)

$ErrorActionPreference = "Stop"
$patterns = @(
    @{ name = "jwt"; pattern = "\beyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\b" },
    @{ name = "provider-key"; pattern = "\b(sk-(proj-)?|ghp_|gho_|glpat-|xoxb-|AKIA|AIza)[A-Za-z0-9_-]{16,}\b" },
    @{ name = "database-url"; pattern = "\bpostgres(ql)?://[^:\s]+:[^@\s]+@[^/\s]+/[^\s]+" },
    @{ name = "bearer"; pattern = "(?i)\bauthorization:\s*bearer\s+[A-Za-z0-9._-]{20,}\b" },
    @{ name = "secret-assignment"; pattern = "(?i)\b(api[_-]?key|secret|token|password|client[_-]?secret)\b\s*[:=]\s*[A-Za-z0-9._+/=-]{30,}" }
)
$allow = @("\$\{[A-Z_][A-Z0-9_]*\}", "\$\{\{[^}]+\}\}", "<REDACTED>", "(?i)placeholder|redacted|your[-_].*here|example|fake|dummy|sample|\.\.\.")
$skip = "\.lock$|\.min\.(js|css)$|(^|[\\/])build[\\/]|(^|[\\/])node_modules[\\/]|(^|[\\/])vendor[\\/]|scan-secrets\.(ps1|sh)$"

function Get-ScanFiles {
    param([string]$SelectedMode, [string[]]$ExplicitPaths)
    switch ($SelectedMode) {
        "staged" { git diff --cached --name-only --diff-filter=ACM }
        "tracked" { git ls-files }
        "all" { git ls-files; git ls-files --others --exclude-standard }
        "paths" { $ExplicitPaths }
    }
}

$scanFiles = @(Get-ScanFiles $Mode $Paths | Sort-Object -Unique)
if ($scanFiles.Count -eq 0) { throw "Secret scan selected no files (mode: $Mode)." }
$found = @()
foreach ($file in $scanFiles) {
    if (-not $file -or -not (Test-Path -LiteralPath $file -PathType Leaf) -or $file -match $skip) { continue }
    $lineNumber = 0
    foreach ($line in @(Get-Content -LiteralPath $file)) {
        $lineNumber++
        if ([string]::IsNullOrWhiteSpace($line) -or ($allow | Where-Object { $line -match $_ })) { continue }
        foreach ($candidate in $patterns) {
            if ($line -match $candidate.pattern) {
                $found += [ordered]@{ file = $file; line = $lineNumber; pattern = $candidate.name; snippet = $line.Trim().Substring(0, [Math]::Min(200, $line.Trim().Length)) }
                break
            }
        }
    }
}
$report = [ordered]@{ matches = $found; files_scanned = $scanFiles.Count; mode = $Mode; status = $(if ($found.Count) { "secrets_detected" } else { "clean" }) }
$report | ConvertTo-Json -Depth 5
if ($found.Count) { exit 1 }
