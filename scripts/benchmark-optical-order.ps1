[CmdletBinding()]
param(
    [ValidateRange(256, 8192)][int]$Size = 3000,
    [ValidateRange(1, 3)][int]$GeneratedFrames = 3,
    [ValidateSet('Fast', 'Medium', 'Slow')][string]$Preset = 'Medium',
    [ValidateRange(1, 100)][int]$FlowPercent = 100,
    [ValidateRange(0.01, 1.0)][double]$CenterWidth = 0.55,
    [ValidateRange(0.01, 1.0)][double]$CenterHeight = 0.45,
    [ValidateRange(0, 100)][int]$PeripheralFlowPercent = 0,
    [ValidateSet('Same', 'Fast', 'Medium', 'Slow')][string]$PeripheralPreset = 'Same',
    [ValidateRange(1, 100)][int]$PeripheralOutputPercent = 50,
    [ValidateSet(1, 2, 4)][int]$Grid = 4,
    [ValidateRange(1, 10000)][int]$Samples = 60,
    [ValidateRange(1, 100)][int]$Rounds = 3,
    [ValidateRange(1, 100)][int]$Warmup = 8,
    [ValidateSet('Full', 'Center', 'Foveated')][string[]]$Workloads = @('Full', 'Center', 'Foveated'),
    [ValidateSet('serial', 'launch-all', 'prepare-all', 'consume-per-eye', 'consume-per-region', 'first-stereo', 'centers-first', 'eye-queues', 'phase-queues', 'two-pair-pipeline')]
    [string[]]$Modes = @('serial', 'launch-all', 'prepare-all', 'consume-per-eye', 'consume-per-region', 'first-stereo', 'centers-first', 'eye-queues', 'phase-queues', 'two-pair-pipeline'),
    [switch]$DebugLayer,
    [switch]$SkipBuild,
    [string]$OutputDirectory
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$Workloads = @($Workloads | ForEach-Object { @{full='Full'; center='Center'; foveated='Foveated'}[$_.ToLowerInvariant()] })
$Modes = @($Modes | ForEach-Object { $_.ToLowerInvariant() })
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot ('build\optical-order-benchmark\runs\' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$benchmarkExe = Join-Path $projectRoot 'build\optical-order-benchmark\bin\optical-order-benchmark.exe'
if (-not $SkipBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio C++ build tools and a Windows SDK.' }
    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild) { throw 'MSBuild was not found.' }
    $msbuildRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $msbuild))
    $availableToolsets = @(Get-ChildItem (Join-Path $msbuildRoot 'Microsoft\VC') -Directory -Filter 'v*' |
        ForEach-Object { Get-ChildItem (Join-Path $_.FullName 'Platforms\x64\PlatformToolsets') -Directory -ErrorAction SilentlyContinue } |
        Select-Object -ExpandProperty Name | Sort-Object -Unique -Descending)
    if (-not $availableToolsets.Count) { throw 'No x64 Visual C++ platform toolset was found.' }
    $toolset = if ($availableToolsets -contains 'v143') { 'v143' } else { $availableToolsets[0] }
    & $msbuild (Join-Path $projectRoot 'tools\optical-order-benchmark.vcxproj') /nologo /nr:false /verbosity:minimal /p:Configuration=Release /p:Platform=x64 "/p:PlatformToolset=$toolset" *> (Join-Path $OutputDirectory 'build.log')
    if ($LASTEXITCODE -ne 0) {
        Get-Content (Join-Path $OutputDirectory 'build.log') -Tail 50
        throw 'Benchmark build failed.'
    }
}
if (-not (Test-Path -LiteralPath $benchmarkExe)) { throw 'Build the benchmark before using -SkipBuild.' }
# Run a private copy so a subsequent benchmark build never replaces a running image.
$runExecutable = Join-Path $OutputDirectory 'optical-order-benchmark.exe'
Copy-Item -LiteralPath $benchmarkExe -Destination $runExecutable -Force
$benchmarkExe = $runExecutable

$resolvedPeripheralPercent = if ($PeripheralFlowPercent) { $PeripheralFlowPercent } else { [Math]::Max(1, [Math]::Floor($FlowPercent / 2)) }
$resolvedPeripheralPreset = if ($PeripheralPreset -eq 'Same') { $Preset } else { $PeripheralPreset }
$metadata = [ordered]@{
    timestamp = (Get-Date).ToString('o'); size = $Size; eyes = 2; generated_frames = $GeneratedFrames
    preset = $Preset; flow_percent = $FlowPercent; grid = $Grid; samples = $Samples; rounds = $Rounds
    warmup = $Warmup; workloads = $Workloads; debug_layer = [bool]$DebugLayer
    center_width = $CenterWidth; center_height = $CenterHeight
    peripheral_flow_percent = $resolvedPeripheralPercent; peripheral_preset = $resolvedPeripheralPreset
    peripheral_output_percent = $PeripheralOutputPercent
    temporal_hints = 'disabled for deterministic ordering comparisons'
    modes = $Modes
    scope = 'GPU-resident synthetic A/B and optional B/C; luma preparation, bidirectional NVOF, interpolation; no game, capture, composition, upload, OpenXR, NVENC, or runtime pacing'
}
$metadata.git_commit = (& git -C $projectRoot rev-parse HEAD 2>$null | Out-String).Trim()
$metadata.binary_sha256 = (Get-FileHash -LiteralPath $benchmarkExe -Algorithm SHA256).Hash
if (Get-Command nvidia-smi -ErrorAction SilentlyContinue) {
    $metadata.gpu_before = (& nvidia-smi --query-gpu=name,driver_version,utilization.gpu,utilization.memory,memory.used,memory.total --format=csv,noheader | Out-String).Trim()
    Write-Host $metadata.gpu_before
}
$metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'metadata.json') -Encoding utf8
Write-Host 'For an idle-GPU comparison, close the game/VR streamer. Run again under load for a separate contention measurement.'
Write-Host 'Single-pair modes have no render-ahead; two-pair-pipeline has exactly two resident pairs. First stereo output age is measured separately.'
$presetIndex = @{ Slow = 0; Medium = 1; Fast = 2 }[$Preset]
$peripheralPresetIndex = @{ Slow = 0; Medium = 1; Fast = 2 }[$resolvedPeripheralPreset]
$benchmarkArgs = @('--size', $Size, '--phases', $GeneratedFrames, '--preset', $presetIndex,
    '--percent', $FlowPercent, '--grid', $Grid, '--samples', $Samples, '--rounds', $Rounds,
    '--center-width', $CenterWidth.ToString([Globalization.CultureInfo]::InvariantCulture),
    '--center-height', $CenterHeight.ToString([Globalization.CultureInfo]::InvariantCulture),
    '--peripheral-percent', $resolvedPeripheralPercent, '--peripheral-preset', $peripheralPresetIndex,
    '--peripheral-output', $PeripheralOutputPercent,
    '--warmup', $Warmup, '--workloads', ($Workloads -join ','), '--modes', ($Modes -join ','), '--output', (Join-Path $OutputDirectory 'samples.csv'))
if ($DebugLayer) { $benchmarkArgs += '--debug' }
& $benchmarkExe @benchmarkArgs 2>&1 | Tee-Object -FilePath (Join-Path $OutputDirectory 'run.log')
if ($LASTEXITCODE -ne 0) { throw "Benchmark failed; inspect $OutputDirectory\run.log" }

function Get-Percentile($Values, [double]$Fraction) {
    $sorted = @($Values | Sort-Object)
    return $sorted[[Math]::Min($sorted.Count - 1, [Math]::Ceiling($Fraction * $sorted.Count) - 1)]
}
$summary = @(Import-Csv -LiteralPath (Join-Path $OutputDirectory 'samples.csv') |
    Group-Object workload, mode | ForEach-Object {
        $gpu = @($_.Group | ForEach-Object { [double]::Parse($_.gpu_ms, [Globalization.CultureInfo]::InvariantCulture) })
        $cpu = @($_.Group | ForEach-Object { [double]::Parse($_.cpu_ms, [Globalization.CultureInfo]::InvariantCulture) })
        $first = @($_.Group | ForEach-Object { [double]::Parse($_.first_output_ms, [Globalization.CultureInfo]::InvariantCulture) })
        [pscustomobject]@{
            Workload = $_.Group[0].workload; Mode = $_.Group[0].mode; Samples = $gpu.Count
            GpuMeanMs = [Math]::Round(($gpu | Measure-Object -Average).Average, 4)
            GpuP50Ms = [Math]::Round((Get-Percentile $gpu 0.50), 4)
            GpuP95Ms = [Math]::Round((Get-Percentile $gpu 0.95), 4)
            CpuMeanMs = [Math]::Round(($cpu | Measure-Object -Average).Average, 4)
            FirstStereoMeanMs = [Math]::Round(($first | Measure-Object -Average).Average, 4)
            PairsPerBatch = [int]$_.Group[0].pairs
        }
    })
$summary | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'summary.csv') -NoTypeInformation
$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8
$summary | Sort-Object Workload, GpuP50Ms | Format-Table -AutoSize
Write-Host "Results: $OutputDirectory"
