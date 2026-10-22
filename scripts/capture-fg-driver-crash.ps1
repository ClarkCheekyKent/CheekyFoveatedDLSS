[CmdletBinding()]
param(
    [string]$ProcessName = 'AC2-Win64-Shipping',
    [int]$WaitSeconds = 180,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repository ('build\fg-diagnostics\capture-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$debugger = (Get-Command cdbX64.exe -ErrorAction Stop).Source
$deadline = (Get-Date).AddSeconds($WaitSeconds)
Write-Host "Waiting for $ProcessName and its NVIDIA D3D11 driver..."
do {
    $candidate = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)
    if ($candidate.Count -gt 1) { throw "More than one $ProcessName process is running; close the extra instance." }
    $target = $null
    if ($candidate.Count -eq 1) {
        try {
            $modules = @($candidate[0].Modules | Select-Object -ExpandProperty ModuleName)
            if ($modules -contains 'nvwgf2umx.dll') { $target = $candidate[0] }
        } catch { Write-Verbose "Process modules not ready: $_" }
    }
    if ($target) { break }
    if ((Get-Date) -ge $deadline) { throw "Timed out waiting for $ProcessName with nvwgf2umx.dll loaded." }
    Start-Sleep -Milliseconds 500
} while ($true)

# Capture the FIRST driver AV, before DCS/UE's exception handlers can replace
# its context. Also catch invalid instruction targets with a driver return
# address (the other observed form of the same command-dispatch corruption).
# Relative output paths keep nested debugger command strings free of quotes.
$commands = @'
.symopt+0x40
sxi eh
sxi out
r @$t0 = nvwgf2umx
r @$t1 = @$t0 + dwo(@$t0 + dwo(@$t0 + 0x3c) + 0x50)
sxe -c ".if (((@rip >= @$t0) and (@rip < @$t1)) or ((poi(@rsp) >= @$t0) and (poi(@rsp) < @$t1))) { .echo NVIDIA FIRST-CHANCE FAULT; .exr -1; r; kn 40; ~*kn 40; .dump /ma nvidia-first-chance.dmp; qd } .else { gn }" av
.echo FG DRIVER CAPTURE ARMED - enable vector FG now
g
'@
$commandFile = Join-Path $OutputDirectory 'capture.cmd'
Set-Content -LiteralPath $commandFile -Value $commands -Encoding ascii
Write-Host "Attaching to PID $($target.Id). Capture directory: $OutputDirectory"
Write-Host 'The full-memory dump stays local. On a matching fault, the debugger saves it and detaches.'
Push-Location -LiteralPath $OutputDirectory
try {
    & $debugger -pd -p $target.Id -y "$repository\bin\Release;C:\Program Files\CheekyFoveatedDLSS\OpenXR" -logo debugger.log -cf $commandFile
    if ($LASTEXITCODE -ne 0) { throw "Debugger exited with code $LASTEXITCODE." }
} finally { Pop-Location }
