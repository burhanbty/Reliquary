; Stable AppId: preserve for every future Reliquary upgrade.
#ifndef AppVersion
  #error AppVersion must be supplied by package.ps1
#endif
#ifndef StageDir
  #error StageDir must be supplied by package.ps1
#endif
#ifndef OutputPath
  #error OutputPath must be supplied by package.ps1
#endif
#ifndef IconFile
  #error IconFile must be the approved Reliquary ICO
#endif

[Setup]
AppId={{DD6EA603-FC48-40C9-ACCC-15914FE7E5B2}
AppName=Reliquary
AppVersion={#AppVersion}
AppVerName=Reliquary {#AppVersion}
AppPublisher=Burhan Talha Yazıcı / BTY
AppPublisherURL=https://www.linkedin.com/in/burhanbty
AppSupportURL=https://github.com/burhanbty/Reliquary
DefaultDirName={localappdata}\Programs\Reliquary
DefaultGroupName=Reliquary
PrivilegesRequired=lowest
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
OutputDir={#OutputPath}
OutputBaseFilename=Reliquary-Setup-v{#AppVersion}-x64
SetupIconFile={#IconFile}
UninstallDisplayName=Reliquary
UninstallDisplayIcon={app}\Reliquary.exe
LicenseFile={#StageDir}\LICENSE.txt
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "turkish"; MessagesFile: "compiler:Languages\Turkish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Reliquary"; Filename: "{app}\Reliquary.exe"; WorkingDir: "{app}"
Name: "{userdesktop}\Reliquary"; Filename: "{app}\Reliquary.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\Reliquary.exe"; Description: "{cm:LaunchProgram,Reliquary}"; Flags: nowait postinstall skipifsilent

; No registry or user-data deletion rules. Inno removes installed files only.
