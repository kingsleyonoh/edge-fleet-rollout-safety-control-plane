$ErrorActionPreference = 'Stop'

cmake --preset dev
cmake --build --preset dev
Write-Output 'EdgeFleet development build is ready at build/dev/edgefleet.exe.'
