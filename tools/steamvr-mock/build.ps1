[CmdletBinding()]
param([switch]$AdapterOnly)
$ErrorActionPreference = 'Stop'
$taskPath = $env:Path
Remove-Item Env:PATH -ErrorAction SilentlyContinue
$env:Path = $taskPath
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$sourceRoot = Join-Path $repoRoot 'build/steamvr-mock/CustomHeadsetOpenVR'
$package = Join-Path $repoRoot 'build/steamvr-mock/package'
$upstream = '21185da63a9e4307ae876b3702a43a43044d451c'
if (!(Test-Path "$sourceRoot/.git")) { throw 'Fetch CustomHeadsetOpenVR and its OpenVR submodule first. See README.md.' }
if ((git -C $sourceRoot rev-parse HEAD) -ne $upstream) { throw "Expected upstream revision $upstream" }
$helper = Join-Path $sourceRoot 'CustomHeadsetOpenVR/src/Helpers/EyeTrackingOutput.cpp'
$header = Join-Path $sourceRoot 'CustomHeadsetOpenVR/src/Helpers/EyeTrackingOutput.h'

# Apply a small, repeatable modification to Sboy's real output helper.
if (!(Select-String -LiteralPath $header -SimpleMatch 'bool valid = true;')) {
    $text = [IO.File]::ReadAllText($header).Replace('double timestamp = 0;', "double timestamp = 0;`n`t`tbool valid = true;")
    [IO.File]::WriteAllText($header, $text)
    $text = [IO.File]::ReadAllText($helper)
    $text = $text.Replace('#include "../Config/ConfigLoader.h"', '// Mock-only build has no headset/display configuration dependency.')
    $text = $text.Replace('vr::VRProperties()->SetBoolProperty(container, vr::Prop_SupportsXrEyeGazeInteraction_Bool, true);', '')
    $text = $text.Replace('initialized = true;', 'initialized = true; vr::VRProperties()->SetBoolProperty(container, vr::Prop_SupportsXrEyeGazeInteraction_Bool, true);')
    $text = $text.Replace('}else if(!eyeTrackingComponentHandle){', '}else{')
    $text = $text.Replace('} else if(!eyeTrackingComponentHandle) {', '} else {')
    $text = $text.Replace('vr::VREyeTrackingData_t eyeTrackingData;', 'vr::VREyeTrackingData_t eyeTrackingData{};')
    $text = $text.Replace('eyeTrackingData.bActive = true;', 'eyeTrackingData.bActive = localData.valid;')
    $text = $text.Replace('eyeTrackingData.bValid = true;', 'eyeTrackingData.bValid = localData.valid;')
    $text = $text.Replace('eyeTrackingData.bTracked = true;', 'eyeTrackingData.bTracked = localData.valid;')
    $text = $text.Replace('driverInput->UpdateEyeTrackingComponent(eyeTrackingComponentHandle, &eyeTrackingData, timeOffset);', @'
const auto result = driverInput->UpdateEyeTrackingComponent(eyeTrackingComponentHandle, &eyeTrackingData, timeOffset);
        static int lastResult = -1;
        if (lastResult != static_cast<int>(result)) {
            DriverLog("Cheeky mock UpdateEyeTrackingComponent result=%d", result);
            lastResult = static_cast<int>(result);
        }
'@)
    $text = [regex]::Replace($text, '(?m)^\s*driverConfigLoader\.diagnosticInfo\.[^\r\n]*', '')
    [IO.File]::WriteAllText($helper, $text)
}
New-Item -ItemType Directory -Force "$package/driver/bin/win64", "$package/probe", "$package/licenses" | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { throw 'Visual C++ build tools were not found.' }
$includes = "$sourceRoot/ThirdParty/openvr/headers;$sourceRoot/CustomHeadsetOpenVR/src/Helpers;$sourceRoot/CustomHeadsetOpenVR/src/Driver;$repoRoot/src;$repoRoot/shared;$repoRoot/third_party/reshade/deps/minhook/include;$repoRoot/third_party/reshade/deps/minhook/src"
function Write-Project($name, $type, $outDir, $files, $dependencies) {
    $items = ($files | ForEach-Object { '<ClCompile Include="' + [Security.SecurityElement]::Escape($_) + '" />' }) -join "`n"
    $xml = @'
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
 <ItemGroup Label="ProjectConfigurations"><ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration></ItemGroup>
 <PropertyGroup Label="Globals"><WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion></PropertyGroup>
 <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
 <PropertyGroup Label="Configuration"><ConfigurationType>TYPE</ConfigurationType><PlatformToolset>v143</PlatformToolset><UseDebugLibraries>false</UseDebugLibraries></PropertyGroup>
 <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />
 <PropertyGroup><OutDir>OUTPUT/</OutDir><IntDir>OUTPUT/obj/</IntDir><TargetName>NAME</TargetName></PropertyGroup>
 <ItemDefinitionGroup>
  <ClCompile><AdditionalIncludeDirectories>INCLUDES</AdditionalIncludeDirectories><PreprocessorDefinitions>WIN32;_WIN32;NOMINMAX;WIN32_LEAN_AND_MEAN;_CRT_SECURE_NO_WARNINGS</PreprocessorDefinitions><LanguageStandard>stdcpp17</LanguageStandard><RuntimeLibrary>MultiThreaded</RuntimeLibrary><WarningLevel>Level4</WarningLevel><Optimization>MaxSpeed</Optimization></ClCompile>
  <Link><AdditionalDependencies>DEPS;%(AdditionalDependencies)</AdditionalDependencies><SubSystem>Console</SubSystem></Link>
 </ItemDefinitionGroup>
 <ItemGroup>FILES</ItemGroup>
 <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
'@
    $xml = $xml.Replace('TYPE',$type).Replace('OUTPUT',[Security.SecurityElement]::Escape($outDir)).Replace('NAME',$name).Replace('INCLUDES',[Security.SecurityElement]::Escape($includes)).Replace('DEPS',[Security.SecurityElement]::Escape($dependencies)).Replace('FILES',$items)
    $project = Join-Path $repoRoot "build/steamvr-mock/$name.vcxproj"
    [IO.File]::WriteAllText($project, $xml)
    & $msbuild $project /nologo /verbosity:minimal /p:Configuration=Release /p:Platform=x64
    if ($LASTEXITCODE) { throw "Build failed: $name" }
}
Write-Project 'adapter-probe' 'Application' "$package/probe" @("$PSScriptRoot/adapter_probe.cpp", "$repoRoot/src/openvr_gaze.cpp", "$repoRoot/shared/gaze_math.cpp", "$repoRoot/third_party/reshade/deps/minhook/src/buffer.c", "$repoRoot/third_party/reshade/deps/minhook/src/hook.c", "$repoRoot/third_party/reshade/deps/minhook/src/trampoline.c", "$repoRoot/third_party/reshade/deps/minhook/src/hde/hde64.c") "$sourceRoot/ThirdParty/openvr/lib/win64/openvr_api.lib;d3d11.lib;d3d12.lib;dxgi.lib;dxguid.lib"
Write-Project 'vtable-tests' 'Application' "$package/tests" @("$PSScriptRoot/vtable_tests.cpp") ''
& "$package/tests/vtable-tests.exe"
if ($LASTEXITCODE) { throw "Vtable regression tests failed: $LASTEXITCODE" }
if ($AdapterOnly) { return }
Write-Project 'driver_cheeky_mock' 'DynamicLibrary' "$package/driver/bin/win64" @("$PSScriptRoot/mock_driver.cpp", $helper, "$sourceRoot/CustomHeadsetOpenVR/src/Driver/DriverLog.cpp") ''
Write-Project 'gaze-probe' 'Application' "$package/probe" @("$PSScriptRoot/probe.cpp") "$sourceRoot/ThirdParty/openvr/lib/win64/openvr_api.lib"
Write-Project 'helper-tests' 'Application' "$package/tests" @("$PSScriptRoot/helper_tests.cpp", $helper) ''
& "$package/tests/helper-tests.exe" "$package/driver/bin/win64/driver_cheeky_mock.dll"
if ($LASTEXITCODE) { throw "Helper/DLL tests failed: $LASTEXITCODE" }
Copy-Item "$sourceRoot/ThirdParty/openvr/bin/win64/openvr_api.dll" "$package/probe/openvr_api.dll"
Copy-Item "$sourceRoot/LICENCE.md" "$package/licenses/CustomHeadsetOpenVR-LICENCE.md"
Copy-Item "$sourceRoot/ThirdParty/openvr/LICENSE" "$package/licenses/OpenVR-LICENSE.txt"
'{"name":"cheeky_mock","alwaysActivate":true,"resourceOnly":false,"redirectsDisplay":false}' | Set-Content "$package/driver/driver.vrdrivermanifest"
if (!(Test-Path "$package/driver/mock.ini")) { "[mock]`nmode=0" | Set-Content "$package/driver/mock.ini" }
git -C $sourceRoot diff --output="$package/sboy-helper.patch" -- CustomHeadsetOpenVR/src/Helpers/EyeTrackingOutput.cpp CustomHeadsetOpenVR/src/Helpers/EyeTrackingOutput.h
Write-Host "Prepared $package (mock starts OFF)."
