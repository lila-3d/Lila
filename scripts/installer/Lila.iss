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
; Lila checks for this and waits instead of starting on half-replaced files.
SetupMutex=LilaSetup
ChangesAssociations=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Registry]
; The values Lila's own file type code (src/io/FileTypes.cpp) writes, so the app and the uninstaller agree on what is Lila's.
Root: HKA; Subkey: "Software\Classes\Lila.lila"; ValueType: string; ValueName: ""; ValueData: "Lila scene"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Lila.lila"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "Lila scene"
Root: HKA; Subkey: "Software\Classes\Lila.lila\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\lila.exe,0"
Root: HKA; Subkey: "Software\Classes\Lila.lila\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\lila.exe"" ""%1"""
Root: HKA; Subkey: "Software\Classes\.lila"; ValueType: string; ValueName: ""; ValueData: "Lila.lila"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.lila\OpenWithProgids"; ValueType: string; ValueName: "Lila.lila"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\Lila.lilaa"; ValueType: string; ValueName: ""; ValueData: "Lila text scene"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Lila.lilaa"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "Lila text scene"
Root: HKA; Subkey: "Software\Classes\Lila.lilaa\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\lila.exe,0"
Root: HKA; Subkey: "Software\Classes\Lila.lilaa\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\lila.exe"" ""%1"""
Root: HKA; Subkey: "Software\Classes\.lilaa"; ValueType: string; ValueName: ""; ValueData: "Lila.lilaa"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.lilaa\OpenWithProgids"; ValueType: string; ValueName: "Lila.lilaa"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\Applications\lila.exe"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "Lila"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Applications\lila.exe\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\lila.exe"" ""%1"""

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; A new version replaces the bundled Python and resources wholesale, so files removed upstream don't linger.
Type: filesandordirs; Name: "{app}\python-runtime"
Type: filesandordirs; Name: "{app}\python-site"
Type: filesandordirs; Name: "{app}\python"
Type: filesandordirs; Name: "{app}\resources"

[Icons]
Name: "{autoprograms}\Lila"; Filename: "{app}\lila.exe"
Name: "{autodesktop}\Lila"; Filename: "{app}\lila.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\lila.exe"; Description: "{cm:LaunchProgram,Lila}"; Flags: nowait postinstall skipifsilent
Filename: "{app}\lila.exe"; Flags: nowait; Check: RelaunchRequested

[UninstallRun]
; Hands back the import formats ticked in Preferences, which only Lila knows about.
Filename: "{app}\lila.exe"; Parameters: "--batch -c ""unregisterFileTypes()"""; Flags: runhidden waituntilterminated; RunOnceId: "LilaFileTypes"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\python-runtime"
Type: filesandordirs; Name: "{app}\python-site"
Type: filesandordirs; Name: "{app}\python"

[Code]
{ Lila leaves this flag when someone opened it while a silent update was installing. }
function RelaunchRequested: Boolean;
var
  Flag: String;
begin
  Flag := GetEnv('TEMP') + '\LilaUpdate\relaunch';
  Result := WizardSilent and FileExists(Flag);
  if Result then
    DeleteFile(Flag);
end;
