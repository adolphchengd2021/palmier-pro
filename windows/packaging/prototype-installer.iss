#ifndef SourceDir
  #error SourceDir is required
#endif
#ifndef OutputDir
  #error OutputDir is required
#endif
#ifndef AppVersion
  #error AppVersion is required
#endif

[Setup]
AppId={{B12503B7-4D6C-4B8D-9B78-9E28BEAEAA62}
AppName=Palmier Pro Windows Prototype
AppVersion={#AppVersion}
AppVerName=Palmier Pro Windows Prototype {#AppVersion}
AppPublisher=Palmier Pro contributors
AppPublisherURL=https://github.com/adolphchengd2021/palmier-pro
AppSupportURL=https://github.com/adolphchengd2021/palmier-pro/issues
DefaultDirName={autopf}\Palmier Pro Windows Prototype
DefaultGroupName=Palmier Pro Windows Prototype
DisableProgramGroupPage=yes
LicenseFile={#SourceDir}\licenses\PalmierPro-GPLv3.txt
InfoBeforeFile={#SourceDir}\THIRD_PARTY_NOTICES.txt
OutputDir={#OutputDir}
OutputBaseFilename=PalmierPro-Windows-Prototype-{#AppVersion}-x64
SetupArchitecture=x64
MinVersion=10.0.19045
PrivilegesRequired=admin
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
UninstallDisplayIcon={app}\PalmierPro.exe
VersionInfoVersion={#AppVersion}
VersionInfoProductName=Palmier Pro Windows Prototype
VersionInfoProductVersion={#AppVersion}
VersionInfoDescription=Unsigned internal Technical MVP installer

[Files]
Source: "{#SourceDir}\redist\vc_redist.x64.exe"; Flags: dontcopy noencryption
Source: "{#SourceDir}\*"; Excludes: "redist\vc_redist.x64.exe"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Palmier Pro Windows Prototype"; Filename: "{app}\PalmierPro.exe"
Name: "{autodesktop}\Palmier Pro Windows Prototype"; Filename: "{app}\PalmierPro.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  Result := '';
  ExtractTemporaryFile('vc_redist.x64.exe');
  if not Exec(
    ExpandConstant('{tmp}\vc_redist.x64.exe'),
    '/install /quiet /norestart',
    '',
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode
  ) then
  begin
    Result := 'The Microsoft Visual C++ Runtime installer could not be started.';
  end
  else if ResultCode = 3010 then
  begin
    NeedsRestart := True;
    Result := 'Restart Windows, then run Palmier Pro Setup again.';
  end
  else if ResultCode <> 0 then
  begin
    Result := 'The Microsoft Visual C++ Runtime installer failed with exit code '
      + IntToStr(ResultCode) + '.';
  end;
end;
