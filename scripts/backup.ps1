param([Parameter(Mandatory = $true)][string]$Destination)
$ErrorActionPreference = 'Stop'

$sqlite = if ($env:SQLITE_PATH) { $env:SQLITE_PATH } else { './data/edgefleet.db' }
$artifacts = if ($env:ARTIFACT_STORE_PATH) { $env:ARTIFACT_STORE_PATH } else { './data/artifacts' }
$traces = if ($env:TRACE_STORE_PATH) { $env:TRACE_STORE_PATH } else { './data/traces' }
$exports = if ($env:EXPORT_STORE_PATH) { $env:EXPORT_STORE_PATH } else { './data/exports' }
$destinationPath = [System.IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force -Path $destinationPath | Out-Null
$binary = if (Test-Path 'build/dev/edgefleet.exe') { 'build/dev/edgefleet.exe' } elseif (Test-Path 'build/production/edgefleet.exe') { 'build/production/edgefleet.exe' } else { 'edgefleet' }
$env:EDGEFLEET_LOG_LEVEL = 'error'
$backupPath = Join-Path $destinationPath 'edgefleet.db'
& $binary backup --out $backupPath | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $backupPath)) { throw 'SQLite backup failed.' }
if (Test-Path $artifacts) { Compress-Archive -Path (Join-Path $artifacts '*') -DestinationPath (Join-Path $destinationPath 'artifacts.zip') -Force }
if (Test-Path $traces) { Compress-Archive -Path (Join-Path $traces '*') -DestinationPath (Join-Path $destinationPath 'traces.zip') -Force }
if (Test-Path $exports) { Compress-Archive -Path (Join-Path $exports '*') -DestinationPath (Join-Path $destinationPath 'exports.zip') -Force }
& $binary evidence --verify | Set-Content -Path (Join-Path $destinationPath 'evidence-verification.json')
