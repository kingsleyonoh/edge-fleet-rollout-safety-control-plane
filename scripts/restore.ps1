param([Parameter(Mandatory = $true)][string]$Source)
$ErrorActionPreference = 'Stop'

$sqlite = if ($env:SQLITE_PATH) { $env:SQLITE_PATH } else { './data/edgefleet.db' }
$artifacts = if ($env:ARTIFACT_STORE_PATH) { $env:ARTIFACT_STORE_PATH } else { './data/artifacts' }
$traces = if ($env:TRACE_STORE_PATH) { $env:TRACE_STORE_PATH } else { './data/traces' }
$exports = if ($env:EXPORT_STORE_PATH) { $env:EXPORT_STORE_PATH } else { './data/exports' }
$sourcePath = [System.IO.Path]::GetFullPath($Source)
if (-not (Test-Path (Join-Path $sourcePath 'edgefleet.db'))) { throw 'Backup database is missing.' }
New-Item -ItemType Directory -Force -Path ([System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($sqlite))) | Out-Null
Copy-Item -LiteralPath (Join-Path $sourcePath 'edgefleet.db') -Destination $sqlite -Force
if (Test-Path (Join-Path $sourcePath 'artifacts.zip')) {
  New-Item -ItemType Directory -Force -Path $artifacts | Out-Null
  Expand-Archive -LiteralPath (Join-Path $sourcePath 'artifacts.zip') -DestinationPath $artifacts -Force
}
foreach ($pair in @(@{ Archive = 'traces.zip'; Path = $traces }, @{ Archive = 'exports.zip'; Path = $exports })) {
  if (Test-Path (Join-Path $sourcePath $pair.Archive)) {
    New-Item -ItemType Directory -Force -Path $pair.Path | Out-Null
    Expand-Archive -LiteralPath (Join-Path $sourcePath $pair.Archive) -DestinationPath $pair.Path -Force
  }
}
$binary = if (Test-Path 'build/dev/edgefleet.exe') { 'build/dev/edgefleet.exe' } else { 'edgefleet' }
$env:EDGEFLEET_LOG_LEVEL = 'error'
& $binary evidence --verify
