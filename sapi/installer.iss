; SSI-263 SAPI -- the Braille Lite 2000 (English and Spanish), the Speak-Out, the Accent-mini and the Accent SA as
; SAPI 5 voices, for any Windows screen reader or program that speaks through SAPI.
;
; It carries the same driver files the NVDA add-ons run -- the emulators, the SSI-263 chip model and each device's
; own firmware -- and python.org's embeddable interpreter that runs them behind the engine DLL.  The firmware is not
; ours: it is here, as in the add-ons, so these machines can talk again, and it will come down if its rights
; holders ask.  A fork of outspoken-nvda's sapi/installer.iss (panthera-speech's).
;
; Build:  powershell -ExecutionPolicy Bypass -File .\sapi\build.ps1
;         ISCC .\sapi\installer.iss
#ifndef StageDir
#define StageDir "..\nvda\dist\sapi"
#endif
#define AppVer "0.6.0"

[Setup]
AppId={{7C3E91A2-5B64-4D0F-9E28-A61D3F84C7B5}
AppName=SSI-263 SAPI voices
AppVersion={#AppVer}
AppPublisher=SSI-263 speech project
AppSupportURL=https://github.com/tgeczy/ssi263-speech
DefaultDirName={autopf}\SSI-263 SAPI
; 64-bit Windows, ARM64 included through its x64 emulation: the bundled Python is amd64 (a 32-bit build later)
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
Compression=lzma2
SolidCompression=yes
OutputDir={#StageDir}\out
OutputBaseFilename=ssi263-sapi-{#AppVer}-setup
DisableProgramGroupPage=yes
UninstallDisplayName=SSI-263 SAPI voices {#AppVer}

[Files]
Source: "{#StageDir}\x86\ssi263_sapi.dll"; DestDir: "{app}\x86"; Flags: ignoreversion
Source: "{#StageDir}\x64\ssi263_sapi.dll"; DestDir: "{app}\x64"; Flags: ignoreversion
Source: "{#StageDir}\ssi_serve.py"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\register.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\settings.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\settings.cmd"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\ssi263_settings.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\synthDrivers\*"; DestDir: "{app}\synthDrivers"; Flags: recursesubdirs ignoreversion
Source: "{#StageDir}\python\*"; DestDir: "{app}\python"; Flags: recursesubdirs ignoreversion

[InstallDelete]
; the driver folders are replaced whole: an old engine file left beside a new driver is a trap
Type: filesandordirs; Name: "{app}\synthDrivers"

[Icons]
; the launcher rather than the batch file: a GUI-subsystem program creates no console, so nothing flashes or
; steals focus before the dialog appears
Name: "{autoprograms}\SSI-263 SAPI settings"; Filename: "{app}\ssi263_settings.exe"; WorkingDir: "{app}"

[Run]
; regsvr32 for both registry views, then one token per voice the server lists.  Every install re-registers:
; these voices carry their firmware, so there is no data folder or choice of voices to preserve.
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\register.ps1"" -Register"; StatusMsg: "Registering the SSI-263 voices..."; Flags: runhidden
Filename: "{app}\ssi263_settings.exe"; Description: "Open SSI-263 SAPI settings"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\register.ps1"" -Unregister"; RunOnceId: "UnregisterSsi263"; Flags: runhidden

[Code]
{ A speech program may still hold our server (python.exe) and the Braille Lite emulator (bns_live.exe) open from
  the install folder: stop those, and only those, so their files can be replaced or removed. }
procedure StopOurProcesses;
var
  Code: Integer;
begin
  Exec('powershell.exe', '-NoProfile -ExecutionPolicy Bypass -Command "Get-Process python,bns_live -ErrorAction SilentlyContinue | Where-Object { $_.Path -like ''' + ExpandConstant('{app}') + '\*'' } | Stop-Process -Force"',
       '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopOurProcesses;
  Result := '';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    StopOurProcesses;
end;
