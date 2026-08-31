[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$binary = if (Test-Path 'build/dev/edgefleet.exe') { 'build/dev/edgefleet.exe' } elseif (Test-Path 'build/production/edgefleet.exe') { 'build/production/edgefleet.exe' } else { throw 'Build edgefleet before replay recovery.' }
$env:EDGEFLEET_LOG_LEVEL = 'error'
$result = & $binary replay-recovery
if ($LASTEXITCODE -ne 0) { throw 'Replay recovery failed.' }
$result
