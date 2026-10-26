; mc2_setup.iss — Inno Setup script for MC2 OpenGL Remastered releases.
;
; Build (from repo):
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" ^
;     /DSourceDir=A:\path\to\deployed-install ^
;     /DAppVersion=0.5.1 ^
;     /DOutputDir=A:\path\to\release_assets ^
;     scripts\installer\mc2_setup.iss
;
; SourceDir = a VERIFIED deployed install (deploy_payload.py output), NOT the repo.
; Output: mc2-setup.exe + mc2-setup-*.bin slices, each <= 2000 MB so every
; piece fits under GitHub's 2 GB per-asset release cap. Users put all pieces
; in one folder and run mc2-setup.exe.
;
; Signing (Azure Artifact Signing): sign SourceDir\mc2.exe BEFORE compiling,
; then sign the produced mc2-setup.exe. Inno also supports SignTool= for
; signing uninstaller/setup internally once signtool is configured.

#ifndef SourceDir
  #error Pass /DSourceDir=<deployed install root>
#endif
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
; VersionInfoVersion must be pure numeric a.b.c.d — pass /DNumericVersion=0.5.1.0
; separately when AppVersion carries a suffix (e.g. "0.5.1-rc1").
#ifndef NumericVersion
  #define NumericVersion "0.0.0.0"
#endif

[Setup]
AppId={{7B4A2C1E-MC2R-4E5F-9A10-2026REMASTER}
AppName=MechCommander 2 OpenGL Remastered
AppVersion={#AppVersion}
AppPublisher=MC2 OpenGL Remastered project
AppPublisherURL=https://github.com/ThranduilsRing/mc2-opengl-remastered
DefaultDirName={autopf}\MC2 OpenGL Remastered
DefaultGroupName=MC2 OpenGL Remastered
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=mc2-setup
; 2 GB GitHub asset cap: span into <=2000 MB slices.
DiskSpanning=yes
DiskSliceSize=2000000000
; LZMA2 solid gives best ratio on game data; ultra is slow — good enough:
Compression=lzma2/max
SolidCompression=yes
; Game writes options.cfg/saves next to exe today — install per-user-writable
; default keeps that working without admin/VirtualStore surprises:
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupIconFile=
WizardStyle=modern
; Version resource on the setup exe itself (AV heuristics like this):
VersionInfoVersion={#NumericVersion}
VersionInfoCompany=MC2 OpenGL Remastered project
VersionInfoDescription=MechCommander 2 OpenGL Remastered installer

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion; \
  Excludes: "debug_state\*,tests\*,*.log,.deployed_manifest.csv,imgui.ini,boot_std*.log"

[Icons]
Name: "{group}\MC2 OpenGL Remastered"; Filename: "{app}\mc2.exe"; WorkingDir: "{app}"
Name: "{group}\MC2 Import campaigns"; Filename: "{app}\mc2-launcher.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\MC2 OpenGL Remastered"; Filename: "{app}\mc2.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"

[Run]
Filename: "{app}\mc2.exe"; Description: "Launch MC2 OpenGL Remastered"; \
  Flags: nowait postinstall skipifsilent; WorkingDir: "{app}"

[UninstallDelete]
; Runtime-generated files the [Files] section doesn't know about:
Type: filesandordirs; Name: "{app}\debug_state"
Type: files; Name: "{app}\options.cfg"
Type: files; Name: "{app}\imgui.ini"
Type: files; Name: "{app}\launcher_env.json"
