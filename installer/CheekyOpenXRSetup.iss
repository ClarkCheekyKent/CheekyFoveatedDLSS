; Compile with scripts/build-installer.ps1 and Inno Setup 6.3 or newer.
#ifndef AppVersion
  #error Build with scripts/build-installer.ps1 to supply the shared release version.
#endif
#ifndef SourceDir
  #define SourceDir "..\bin\Release"
#endif

[Setup]
AppId={{C12B3398-1446-49AE-B574-407638F641CA}
AppName=Cheeky OpenXR Support
AppVersion={#AppVersion}
AppPublisher=Cheeky Foveated DLSS
DefaultDirName={commonpf}\CheekyFoveatedDLSS\OpenXR
DisableDirPage=yes
DisableWelcomePage=no
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
UninstallDisplayName=Cheeky OpenXR Support
OutputDir=..\bin\installer
OutputBaseFilename=CheekyOpenXRSetup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\LICENSE
CloseApplications=yes
RestartApplications=no
; Keep the original identity, install path and mutex for upgrades from the old name.
SetupMutex=CheekyOpenXREyeTrackingSetup

[Files]
Source: "{#SourceDir}\CheekyOpenXRLayer.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\XR_APILAYER_CHEEKY_foveated_dlss.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"
Source: "..\third_party\openxr\LICENSE.txt"; DestDir: "{app}"; DestName: "OpenXR-LICENSE.txt"

[Registry]
; Remove only our manifest value on uninstall, never the shared OpenXR key.
Root: HKLM64; Subkey: "SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"; ValueType: dword; ValueName: "{app}\XR_APILAYER_CHEEKY_foveated_dlss.json"; ValueData: "0"; Flags: uninsdeletevalue

[Messages]
WelcomeLabel2=This installs shared OpenXR support for Cheeky Foveated DLSS: automatic stereo alignment, eye calibration and eye tracking.%n%nClose your OpenXR games before installing or updating.%n%nInstall the Cheeky ReShade add-on or UEVR plugin separately for each game. This installer installs only the shared OpenXR layer.
FinishedLabel=Cheeky OpenXR support has been installed.%n%nStart or restart your game with the Cheeky ReShade add-on or UEVR plugin. Open Diagnostics > Eye calibration to check automatic stereo calibration.%n%nStereo alignment and calibration do not require an eye tracker. Eye tracking requires a compatible headset and OpenXR runtime. This installation is shared by all your OpenXR games.
