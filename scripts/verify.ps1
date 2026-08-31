$ErrorActionPreference = 'Stop'
cmake --build --preset dev
ctest --preset dev --output-on-failure
if (Get-Command clang-format -ErrorAction SilentlyContinue) { cmake --build build/dev --target format }
if (Test-Path fixtures/benchmarks/v1/manifest.json) { Get-Content fixtures/benchmarks/v1/manifest.json | ConvertFrom-Json | Out-Null }
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/scan-secrets.ps1 -Mode all | Write-Output
Write-Output 'EdgeFleet build, CTest, and secret scan passed.'
