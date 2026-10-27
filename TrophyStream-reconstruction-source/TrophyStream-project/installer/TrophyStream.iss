#define AppName "TrophyStream"
#define AppVersion "0.2.0"
#define AppPublisher "TrophyStream"
#define AgentDir "..\agent"
#define PluginDir "..\release\trophy-stream"

[Setup]
AppId={{A2E1D482-24F1-4B3E-AE29-6F4D34D1D23B}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\TrophyStream
DefaultGroupName=TrophyStream
OutputDir=output
OutputBaseFilename=TrophyStream-Setup
ArchitecturesInstallIn64BitMode=x64
PrivilegesRequired=admin
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes

[Files]
Source: "{#PluginDir}\bin\64bit\trophy-stream.dll"; DestDir: "{commonappdata}\obs-studio\plugins\trophy-stream\bin\64bit"; Flags: ignoreversion
Source: "{#PluginDir}\data\*"; DestDir: "{commonappdata}\obs-studio\plugins\trophy-stream\data"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#AgentDir}\*"; DestDir: "{app}\agent"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: ".env,node_modules,*.log"
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Dirs]
Name: "{app}\agent"

[Icons]
Name: "{group}\TrophyStream — Agent Discord"; Filename: "{cmd}"; Parameters: "/c cd /d ""{app}\agent"" && npm start"; WorkingDir: "{app}\agent"
Name: "{group}\TrophyStream — Configuration locale"; Filename: "http://127.0.0.1:3020/"

[Run]
Filename: "{cmd}"; Parameters: "/c npm install --omit=dev"; WorkingDir: "{app}\agent"; StatusMsg: "Installation des dépendances du bot local..."; Flags: runhidden waituntilterminated; Check: NpmExists
Filename: "http://127.0.0.1:3020/"; Description: "Ouvrir la configuration locale TrophyStream"; Flags: postinstall shellexec skipifsilent

[Code]
function NpmExists(): Boolean;
var ResultCode: Integer;
begin
  Result := Exec(ExpandConstant('{cmd}'), '/c npm --version', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;
