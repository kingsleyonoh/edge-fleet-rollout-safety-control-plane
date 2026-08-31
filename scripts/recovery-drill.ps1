[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Destination)

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptRoot
$sourceSqlite = [System.IO.Path]::GetFullPath($(if ($env:SQLITE_PATH) { $env:SQLITE_PATH } else { Join-Path $repoRoot 'data/edgefleet.db' }))
$sourceArtifacts = [System.IO.Path]::GetFullPath($(if ($env:ARTIFACT_STORE_PATH) { $env:ARTIFACT_STORE_PATH } else { Join-Path $repoRoot 'data/artifacts' }))
$sourceTraces = [System.IO.Path]::GetFullPath($(if ($env:TRACE_STORE_PATH) { $env:TRACE_STORE_PATH } else { Join-Path $repoRoot 'data/traces' }))
$sourceExports = [System.IO.Path]::GetFullPath($(if ($env:EXPORT_STORE_PATH) { $env:EXPORT_STORE_PATH } else { Join-Path $repoRoot 'data/exports' }))
$destinationRoot = [System.IO.Path]::GetFullPath($Destination)
if ($destinationRoot.TrimEnd('\') -eq ([System.IO.Path]::GetDirectoryName($sourceSqlite)).TrimEnd('\')) { throw 'Recovery destination must be separate from the source database directory.' }
$backupRoot = Join-Path $destinationRoot 'backup'
$restoreRoot = Join-Path $destinationRoot 'restored'
$restoreSqlite = Join-Path $restoreRoot 'edgefleet.db'
$restoreArtifacts = Join-Path $restoreRoot 'artifacts'
$restoreTraces = Join-Path $restoreRoot 'traces'
$restoreExports = Join-Path $restoreRoot 'exports'
New-Item -ItemType Directory -Force -Path $destinationRoot,$backupRoot,$restoreRoot | Out-Null

$binary = if (Test-Path (Join-Path $repoRoot 'build/dev/edgefleet.exe')) { Join-Path $repoRoot 'build/dev/edgefleet.exe' } elseif (Test-Path (Join-Path $repoRoot 'build/production/edgefleet.exe')) { Join-Path $repoRoot 'build/production/edgefleet.exe' } else { throw 'Build edgefleet before running a recovery drill.' }
$env:EDGEFLEET_LOG_LEVEL = 'error'

function Invoke-JsonBinary([string[]]$Arguments) {
  $stderr = Join-Path $destinationRoot 'command.stderr.log'
  $output = & $binary @Arguments 2> $stderr | Out-String
  if ($LASTEXITCODE -ne 0) { throw "edgefleet $($Arguments -join ' ') failed. See $stderr" }
  $line = ($output -split "`r?`n" | Where-Object { $_.Trim().Length -gt 0 } | Select-Object -Last 1)
  if ([string]::IsNullOrWhiteSpace($line)) { throw "edgefleet $($Arguments -join ' ') returned no JSON." }
  return ($line | ConvertFrom-Json)
}

function Get-TreeHashes([string]$Path) {
  $hashes = [ordered]@{}
  if (-not (Test-Path $Path)) { return $hashes }
  $rootPath = [System.IO.Path]::GetFullPath($Path)
  Get-ChildItem -LiteralPath $rootPath -File -Recurse | Sort-Object FullName | ForEach-Object {
    $relative = $_.FullName.Substring($rootPath.Length).TrimStart([char]92, [char]47).Replace([char]92, [char]47)
    $hashes[$relative] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
  }
  return $hashes
}

$env:SQLITE_PATH = $sourceSqlite
$env:ARTIFACT_STORE_PATH = $sourceArtifacts
$env:TRACE_STORE_PATH = $sourceTraces
$env:EXPORT_STORE_PATH = $sourceExports
& (Join-Path $scriptRoot 'backup.ps1') -Destination $backupRoot
$sourceCheck = Invoke-JsonBinary @('recovery-check', '--require-completed')

$env:SQLITE_PATH = $restoreSqlite
$env:ARTIFACT_STORE_PATH = $restoreArtifacts
$env:TRACE_STORE_PATH = $restoreTraces
$env:EXPORT_STORE_PATH = $restoreExports
& (Join-Path $scriptRoot 'restore.ps1') -Source $backupRoot
$restoredDatabaseHash = (Get-FileHash -LiteralPath $restoreSqlite -Algorithm SHA256).Hash.ToLowerInvariant()
$replayRecovery = Invoke-JsonBinary @('replay-recovery')
$restoredCheck = Invoke-JsonBinary @('recovery-check', '--require-completed')

$databaseBytesMatch = (Get-FileHash -LiteralPath (Join-Path $backupRoot 'edgefleet.db') -Algorithm SHA256).Hash.ToLowerInvariant() -eq $restoredDatabaseHash
$artifactBytesMatch = (ConvertTo-Json (Get-TreeHashes $sourceArtifacts) -Compress -Depth 20) -eq (ConvertTo-Json (Get-TreeHashes $restoreArtifacts) -Compress -Depth 20)
$sourceDigests = @($sourceCheck.tenants | ForEach-Object { $_.latest_completed_release.decision_digest })
$restoredDigests = @($restoredCheck.tenants | ForEach-Object { $_.latest_completed_release.decision_digest })
$decisionMatch = (ConvertTo-Json $sourceDigests -Compress) -eq (ConvertTo-Json $restoredDigests -Compress)
$status = if ($databaseBytesMatch -and $artifactBytesMatch -and $decisionMatch -and $restoredCheck.status -eq 'ok') { 'passed' } else { 'failed' }
$result = [ordered]@{
  status = $status
  source = $sourceCheck
  restored = $restoredCheck
  database_bytes_match = $databaseBytesMatch
  artifact_bytes_match = $artifactBytesMatch
  latest_completed_decision_match = $decisionMatch
  replay_recovery = $replayRecovery
  backup_path = $backupRoot
  restored_path = $restoreRoot
}
$resultPath = Join-Path $destinationRoot 'recovery-result.json'
$result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $resultPath -Encoding UTF8
$result | ConvertTo-Json -Depth 30
if ($status -ne 'passed') { throw "Recovery drill failed. See $resultPath" }
