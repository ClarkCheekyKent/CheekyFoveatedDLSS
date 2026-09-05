; Compile with scripts/build-installer.ps1 and Inno Setup 6.3 or newer.
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\bin\Release"
#endif

[Setup]
AppId={{C12B3398-1446-49AE-B574-407638F641CA}
AppName=Cheeky OpenXR Eye Tracking
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
UninstallDisplayName=Cheeky OpenXR Eye Tracking
OutputDir=..\bin\installer
OutputBaseFilename=CheekyEyeTrackingSetup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\LICENSE
CloseApplications=yes
RestartApplications=no
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
WelcomeLabel2=This installs shared OpenXR eye-tracking support for Cheeky Foveated DLSS.%n%nClose your OpenXR games before installing or updating.%n%nFor each game, install ReShade with full add-on support and copy CheekyFoveatedDLSS.addon64 into the game folder separately. This installer does not install ReShade or the add-on.
FinishedLabel=OpenXR eye-tracking support has been installed.%n%nStart or restart your game, open the Cheeky Foveated DLSS add-on in ReShade, and select Foveation center > OpenXR gaze.%n%nEye tracking requires a supported OpenXR game and runtime. This installation is shared by all your games.
