[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$BaseUrl,
  [Parameter(Mandatory = $true)][string]$DeviceIds,
  [Parameter(Mandatory = $true)][string]$DeviceSecret,
  [Parameter(Mandatory = $true)][string]$OperatorApiKey,
  [int]$RequestCount = 500,
  [int]$ThrottleLimit = 16,
  [int]$DeviceSequence = 1,
  [string]$OutputPath = ''
)

$ErrorActionPreference = 'Stop'
if ($RequestCount -lt 500) { throw 'RequestCount must be at least 500 for the locked reference workload.' }
if ($ThrottleLimit -lt 1) { throw 'ThrottleLimit must be positive.' }
if ($DeviceSequence -lt 1) { throw 'DeviceSequence must be positive.' }
Write-Verbose "Reference device identifier input length=$(([string]$DeviceIds).Length)"

Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Linq;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;

public sealed class EdgeFleetHttpSample {
  public int Status { get; set; }
  public double Milliseconds { get; set; }
}

public static class EdgeFleetReferenceLoad {
  private static readonly HttpClient Client = new HttpClient(new HttpClientHandler { UseProxy = false, UseCookies = true }) { Timeout = TimeSpan.FromSeconds(30) };

  private static string Hex(byte[] bytes) => Convert.ToHexString(bytes).ToLowerInvariant();
  private static string Signature(string secret, string method, string path, int sequence, string bodyDigest) => Hex(new HMACSHA256(Encoding.UTF8.GetBytes(secret)).ComputeHash(Encoding.UTF8.GetBytes(method + " " + path + " " + sequence + " " + bodyDigest)));

  public static EdgeFleetHttpSample[] Desired(string baseUrl, string secret, string deviceIdsCsv, int sequence, int count, int throttleLimit) {
    var deviceIds = deviceIdsCsv.Split(',', StringSplitOptions.RemoveEmptyEntries).Select(deviceId => deviceId.Trim()).ToArray();
    return Run(count, throttleLimit, i => {
      var deviceId = deviceIds[i % deviceIds.Length];
      var request = new HttpRequestMessage(HttpMethod.Get, baseUrl.TrimEnd('/') + "/api/agent/v1/desired-state");
      request.Headers.TryAddWithoutValidation("X-Device-Id", deviceId);
      request.Headers.TryAddWithoutValidation("X-Device-Key-Version", "1");
      request.Headers.TryAddWithoutValidation("X-Device-Sequence", sequence.ToString());
      request.Headers.TryAddWithoutValidation("X-Device-Secret", secret);
      request.Headers.TryAddWithoutValidation("X-Device-Signature", Signature(secret, "GET", "/api/agent/v1/desired-state", sequence, Hex(SHA256.HashData(Array.Empty<byte>()))));
      return request;
    });
  }

  public static EdgeFleetHttpSample[] Reports(string baseUrl, string deviceSecret, string deviceIdsCsv, int sequence, int count, int throttleLimit) {
    var deviceIds = deviceIdsCsv.Split(',', StringSplitOptions.RemoveEmptyEntries).Select(deviceId => deviceId.Trim()).ToArray();
    return Run(count, throttleLimit, i => {
      var deviceId = deviceIds[i % deviceIds.Length];
      var body = "{\"device_id\":\"" + deviceId + "\",\"report_id\":\"reference-report-" + sequence + "-" + i + "\",\"report_sequence\":" + sequence + ",\"report_type\":\"observation\",\"observed_generation\":0,\"health\":{}}";
      var bodyDigest = Hex(SHA256.HashData(Encoding.UTF8.GetBytes(body)));
      var signature = Signature(deviceSecret, "POST", "/api/agent/v1/reports", sequence, bodyDigest);
      var request = new HttpRequestMessage(HttpMethod.Post, baseUrl.TrimEnd('/') + "/api/agent/v1/reports") { Content = new StringContent(body, Encoding.UTF8, "application/json") };
      request.Headers.TryAddWithoutValidation("X-Device-Id", deviceId);
      request.Headers.TryAddWithoutValidation("X-Device-Key-Version", "1");
      request.Headers.TryAddWithoutValidation("X-Device-Secret", deviceSecret);
      request.Headers.TryAddWithoutValidation("X-Device-Sequence", sequence.ToString());
      request.Headers.TryAddWithoutValidation("X-Device-Signature", signature);
      return request;
    });
  }

  public static EdgeFleetHttpSample[] OperatorReads(string baseUrl, string apiKey, int count, int throttleLimit) {
    using var sessionRequest = new HttpRequestMessage(HttpMethod.Post, baseUrl.TrimEnd('/') + "/auth/session") { Content = new StringContent("{\"api_key\":\"" + apiKey + "\"}", Encoding.UTF8, "application/json") };
    using var sessionResponse = Client.SendAsync(sessionRequest).GetAwaiter().GetResult();
    if ((int)sessionResponse.StatusCode != 204) throw new InvalidOperationException("Operator session bootstrap failed with status " + (int)sessionResponse.StatusCode);
    return Run(count, throttleLimit, i => {
      var request = new HttpRequestMessage(HttpMethod.Get, baseUrl.TrimEnd('/') + "/api/fleets");
      return request;
    });
  }

  public static EdgeFleetHttpSample[] InitialHtml(string baseUrl, int count, int throttleLimit) {
    return Run(count, throttleLimit, i => new HttpRequestMessage(HttpMethod.Get, baseUrl.TrimEnd('/') + "/login"));
  }

  private static EdgeFleetHttpSample[] Run(int count, int throttleLimit, Func<int, HttpRequestMessage> requestFactory) {
    using var gate = new System.Threading.SemaphoreSlim(throttleLimit, throttleLimit);
    var tasks = Enumerable.Range(0, count).Select(async i => {
      await gate.WaitAsync().ConfigureAwait(false);
      var stopwatch = Stopwatch.StartNew();
      try {
        using var request = requestFactory(i);
        using var response = await Client.SendAsync(request).ConfigureAwait(false);
        await response.Content.ReadAsByteArrayAsync().ConfigureAwait(false);
        stopwatch.Stop();
        return new EdgeFleetHttpSample { Status = (int)response.StatusCode, Milliseconds = stopwatch.Elapsed.TotalMilliseconds };
      } catch {
        stopwatch.Stop();
        return new EdgeFleetHttpSample { Status = 0, Milliseconds = stopwatch.Elapsed.TotalMilliseconds };
      } finally { gate.Release(); }
    }).ToArray();
    Task.WaitAll(tasks);
    return tasks.Select(task => task.Result).ToArray();
  }
}
'@

function Get-Stats([string]$Name, [EdgeFleetHttpSample[]]$Samples, [int]$ExpectedStatus) {
  $ordered = @($Samples | Sort-Object Milliseconds)
  $p95Index = [Math]::Min($ordered.Count - 1, [Math]::Ceiling($ordered.Count * 0.95) - 1)
  $p95 = [double]$ordered[$p95Index].Milliseconds
  $successful = @($ordered | Where-Object Status -eq $ExpectedStatus).Count
  $statusCounts = [ordered]@{}
  $ordered | Group-Object Status | Sort-Object Name | ForEach-Object { $statusCounts[[string]$_.Name] = $_.Count }
  $result = [ordered]@{ name = $Name; requests = $ordered.Count; expected_status = $ExpectedStatus; expected_status_count = $successful; status_counts = $statusCounts; p95_ms = [Math]::Round($p95, 3); max_ms = [Math]::Round([double]$ordered[-1].Milliseconds, 3) }
  return $result
}

$deviceCount = ([regex]::Matches([string]$DeviceIds, '[^,\r\n]+')).Count
if ($deviceCount -eq 0) { throw 'DeviceIds must contain at least one device ID.' }
Write-Verbose "Parsed reference device identifiers=$deviceCount"
$desired = Get-Stats 'desired_state_get' ([EdgeFleetReferenceLoad]::Desired($BaseUrl, $DeviceSecret, [string]$DeviceIds, $DeviceSequence, $RequestCount, $ThrottleLimit)) 204
$reports = Get-Stats 'device_report_post' ([EdgeFleetReferenceLoad]::Reports($BaseUrl, $DeviceSecret, [string]$DeviceIds, $DeviceSequence, $RequestCount, $ThrottleLimit)) 202
$operatorReads = Get-Stats 'operator_read' ([EdgeFleetReferenceLoad]::OperatorReads($BaseUrl, $OperatorApiKey, 100, $ThrottleLimit)) 200
$initialHtml = Get-Stats 'initial_html' ([EdgeFleetReferenceLoad]::InitialHtml($BaseUrl, 100, $ThrottleLimit)) 200
$report = [ordered]@{ status = if ($desired.expected_status_count -eq $RequestCount -and $reports.expected_status_count -eq $RequestCount -and $operatorReads.expected_status_count -eq 100 -and $initialHtml.expected_status_count -eq 100 -and $desired.p95_ms -lt 100 -and $reports.p95_ms -lt 150 -and $operatorReads.p95_ms -lt 200 -and $initialHtml.p95_ms -lt 400) { 'passed' } else { 'failed' }; host = [Environment]::MachineName; processor_count = [Environment]::ProcessorCount; device_count = $deviceCount; measurements = @($desired, $reports, $operatorReads, $initialHtml); request_count = $RequestCount; throttle_limit = $ThrottleLimit; device_sequence = $DeviceSequence }
$json = $report | ConvertTo-Json -Depth 10
if ($OutputPath) { $reportDirectory = Split-Path -Parent ([System.IO.Path]::GetFullPath($OutputPath)); New-Item -ItemType Directory -Force -Path $reportDirectory | Out-Null; $json | Set-Content -LiteralPath $OutputPath -Encoding UTF8 }
$json
if ($report.status -ne 'passed') { throw 'Reference HTTP p95 targets did not pass.' }
