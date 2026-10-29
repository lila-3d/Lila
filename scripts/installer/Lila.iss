; Lila installer: per-user, no admin. Built by build.ps1, which stages the install and passes the defines below.
#ifndef AppVersion
  #error Build with scripts\installer\build.ps1
#endif

[Setup]
AppId={{6B1E7C2A-4F3D-4A8E-9C51-2D7E8B0F4A13}
AppName=Lila
AppVersion={#AppVersion}
AppVerName=Lila {#AppVersion}
AppPublisher=Jared Taylor
AppPublisherURL=https://lila-3d.github.io/
AppSupportURL=https://lila-3d.github.io/docs/#/support
DefaultDirName={autopf}\Lila
DefaultGroupName=Lila
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile={#SourceDir}\LICENSE
SetupIconFile={#RepoDir}\resources\icons\lila.ico
UninstallDisplayIcon={app}\lila.exe
OutputDir={#OutputDir}
OutputBaseFilename=Lila-{#AppVersion}-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; A new version replaces the bundled Python and resources wholesale, so files removed upstream don't linger.
Type: filesandordirs; Name: "{app}\python-runtime"
Type: filesandordirs; Name: "{app}\python"
Type: filesandordirs; Name: "{app}\resources"

[Icons]
Name: "{autoprograms}\Lila"; Filename: "{app}\lila.exe"
Name: "{autodesktop}\Lila"; Filename: "{app}\lila.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\lila.exe"; Description: "{cm:LaunchProgram,Lila}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\python-runtime"
Type: filesandordirs; Name: "{app}\python"
