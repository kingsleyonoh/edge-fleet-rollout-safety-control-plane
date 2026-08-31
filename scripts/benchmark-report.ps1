[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Manifest,
    [string]$OutputDirectory = "./data/benchmark-reports"
)

$ErrorActionPreference = "Stop"
$manifestObject = Get-Content -Raw -LiteralPath $Manifest | ConvertFrom-Json
if ($manifestObject.schema_version -ne "v1" -or $manifestObject.expected_case_count -ne 108) { throw "The frozen v1 corpus must declare exactly 108 cells." }
$binary = if (Test-Path "./build/dev/edgefleet.exe") { "./build/dev/edgefleet.exe" } elseif (Test-Path "./build/production/edgefleet.exe") { "./build/production/edgefleet.exe" } else { throw "Build edgefleet before generating a report." }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$env:EDGEFLEET_LOG_LEVEL = "error"
$result = & $binary benchmark --corpus $Manifest | ConvertFrom-Json
if ($result.cell_count -ne 108) { throw "Benchmark did not produce 108 unique cells." }
$keys = @($result.cells | ForEach-Object { "$($_.scenario)|$($_.seed)|$($_.strategy)" } | Sort-Object -Unique)
if ($keys.Count -ne 108) { throw "Benchmark cell identity is not unique." }
$requiredMetrics = @('unhealthy_exposure_fraction', 'max_concurrent_failed_devices', 'time_to_detection_seconds', 'time_to_pause_seconds', 'rollback_convergence_fraction', 'healthy_convergence_fraction', 'stranded_devices', 'false_rollback')
foreach ($cell in @($result.cells)) {
    foreach ($metric in $requiredMetrics) {
        if (-not ($cell.metrics.PSObject.Properties.Name -contains $metric)) { throw "Benchmark cell $($cell.scenario)/$($cell.seed)/$($cell.strategy) is missing metric $metric." }
    }
}
$result | ConvertTo-Json -Depth 20 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $OutputDirectory "report.json")
Write-Output ([ordered]@{ status = "completed"; corpus = $manifestObject.corpus_version; cells = $result.cell_count; output = [System.IO.Path]::GetFullPath($OutputDirectory) } | ConvertTo-Json)
