[CmdletBinding()]
param(
    [ValidateRange(1, 15)]
    [int]$Seconds = 6
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskOutput = Join-Path $taskRoot 'build'
$taskTag = 'Cheeky-FG-GPU-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
$taskTrace = Join-Path $taskOutput ($taskTag + '.etl')
$taskLog = Join-Path $taskOutput ($taskTag + '.txt')
$taskWpr = Join-Path $env:SystemRoot 'System32\wpr.exe'

$taskIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
$taskPrincipal = [Security.Principal.WindowsPrincipal]::new($taskIdentity)
if (-not $taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Windows GPU recording requires an administrator PowerShell. No recording has been started.'
}
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null

# Do not join, stop or cancel a pre-existing user recording. On an unfamiliar
# status response, fail closed rather than assuming that the recorder is idle.
$taskStatus = (& $taskWpr -status 2>&1 | Out-String)
$taskStatus | Set-Content -LiteralPath $taskLog -Encoding UTF8
if ($taskStatus -notmatch 'WPR is not recording') {
    throw "Could not confirm an idle Windows recorder. Existing traces are untouched. See $taskLog"
}

"GPU-only capture, duration=$Seconds seconds; UTC=$([DateTime]::UtcNow.ToString('o'))" |
    Add-Content -LiteralPath $taskLog -Encoding UTF8
Get-Process -Name HogwartsLegacy -ErrorAction SilentlyContinue |
    Select-Object Id, ProcessName, StartTime | Format-Table -AutoSize | Out-String |
    Add-Content -LiteralPath $taskLog -Encoding UTF8

$taskStart = (& $taskWpr -start GPU -filemode 2>&1 | Out-String)
$taskStartCode = $LASTEXITCODE
$taskStart | Add-Content -LiteralPath $taskLog -Encoding UTF8
if ($taskStartCode -ne 0) {
    throw "GPU recording failed to start (exit $taskStartCode). No stop/cancel was issued. See $taskLog"
}

try {
    Start-Sleep -Seconds $Seconds
} finally {
    # Only reached after this helper successfully started its own recording.
    $taskStop = (& $taskWpr -stop $taskTrace 'Cheeky DLSS-FG preparation investigation' 2>&1 | Out-String)
    $taskStopCode = $LASTEXITCODE
    $taskStop | Add-Content -LiteralPath $taskLog -Encoding UTF8
    if ($taskStopCode -ne 0) {
        throw "Windows could not save the GPU trace (exit $taskStopCode). See $taskLog"
    }
}
Get-Item -LiteralPath $taskTrace | Select-Object FullName, Length, LastWriteTime |
    Format-List | Out-String | Add-Content -LiteralPath $taskLog -Encoding UTF8
Write-Output $taskTrace
