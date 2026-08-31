$ErrorActionPreference = 'Stop'

$binary = if (Test-Path 'build/dev/edgefleet.exe') { 'build/dev/edgefleet.exe' } else { 'edgefleet' }
& $binary migrate
