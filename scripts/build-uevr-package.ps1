[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9.-]*$')]
    [string]$Label = "build",
    [string]$IsccPath,
    [switch]$SkipTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# The shared installer uses Release artifacts. Build everything once so the
# adapter, runtime and installer payload all come from the same build.
& (Join-Path $PSScriptRoot 'build.ps1') -Configuration Release -SkipTests:$SkipTests
& (Join-Path $PSScriptRoot 'build-installer.ps1') -SkipBuild -IsccPath $IsccPath
& (Join-Path $PSScriptRoot 'package-uevr.ps1') -Configuration Release -Label $Label -IncludeOpenXRSetup
