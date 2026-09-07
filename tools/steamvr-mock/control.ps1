[CmdletBinding()]
param([ValidateSet('off','center','sweep','dropout','probe','register','unregister','status')][string]$Action = 'status')
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$package = Join-Path $repoRoot 'build/steamvr-mock/package'
$driver = Join-Path $package 'driver'
$paths = Join-Path $env:LOCALAPPDATA 'openvr/openvrpaths.vrpath'
$vrpaths = Get-Content -LiteralPath $paths -Raw | ConvertFrom-Json
$runtime = $vrpaths.runtime[0]
$registrar = Join-Path $runtime 'bin/win64/vrpathreg.exe'
$modes = @{off=0;center=1;sweep=2;dropout=3}
if ($modes.ContainsKey($Action)) {
    "[mock]`nmode=$($modes[$Action])" | Set-Content -LiteralPath "$driver/mock.ini"
    Write-Host "Mock gaze: $Action (live within 250 ms)."
} elseif ($Action -eq 'probe') {
    if (!(Get-Process vrserver -ErrorAction SilentlyContinue)) { throw 'Start SteamVR through Virtual Desktop first.' }
    $log = Join-Path $package ('probe-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.txt')
    & "$package/probe/gaze-probe.exe" | Tee-Object -FilePath $log
    Write-Host "Saved $log"
} elseif ($Action -eq 'register' -or $Action -eq 'unregister') {
    if (Get-Process vrserver -ErrorAction SilentlyContinue) { throw 'Exit SteamVR before changing driver registration.' }
    if (!(Test-Path "$driver/bin/win64/driver_cheeky_mock.dll")) { throw 'Build the mock first.' }
    if ($Action -eq 'register') {
        if (!(Test-Path "$package/openvrpaths-before.vrpath")) { Copy-Item -LiteralPath $paths -Destination "$package/openvrpaths-before.vrpath" }
        & $registrar adddriver $driver
    } else {
        "[mock]`nmode=0" | Set-Content -LiteralPath "$driver/mock.ini"
        & $registrar removedriver $driver
    }
    if ($LASTEXITCODE) { throw "Registration command failed: $LASTEXITCODE" }
    & $registrar show
} else {
    Get-Content -LiteralPath "$driver/mock.ini"
    & $registrar show
    $log = Join-Path $vrpaths.log[0] 'vrserver.txt'
    if (Test-Path $log) { Get-Content -LiteralPath $log -Tail 3000 | Select-String 'Cheeky mock|EyeTrackingOutput|cheeky_mock' | Select-Object -Last 20 }
}
