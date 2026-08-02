#ifndef SourceDir
  #error SourceDir is required
#endif
#ifndef OutputDir
  #error OutputDir is required
#endif
#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef VcRedistVersion
  #error VcRedistVersion is required
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
function InstalledVcRuntimeIsCurrent: Boolean;
var
  InstalledVersionText: String;
  InstalledVersion: Int64;
  RequiredVersion: Int64;
begin
  Result := False;
  if not RegQueryStringValue(
    HKLM64,
    'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64',
    'Version',
    InstalledVersionText
  ) then
  begin
    Log('Microsoft Visual C++ Runtime x64 registry version is unavailable.');
    Exit;
  end;
  if (Length(InstalledVersionText) > 0)
    and ((InstalledVersionText[1] = 'v') or (InstalledVersionText[1] = 'V')) then
  begin
    Delete(InstalledVersionText, 1, 1);
  end;
  if not StrToVersion(InstalledVersionText, InstalledVersion) then
  begin
    Log('Microsoft Visual C++ Runtime x64 registry version is malformed.');
    Exit;
  end;
  if not StrToVersion('{#VcRedistVersion}', RequiredVersion) then
  begin
    Log('Bundled Microsoft Visual C++ Runtime version is malformed.');
    Exit;
  end;
  Result := ComparePackedVersion(InstalledVersion, RequiredVersion) >= 0;
  if Result then
    Log(
      'Microsoft Visual C++ Runtime x64 installed=' + InstalledVersionText
        + ', required={#VcRedistVersion}, current=True'
    )
  else
    Log(
      'Microsoft Visual C++ Runtime x64 installed=' + InstalledVersionText
        + ', required={#VcRedistVersion}, current=False'
    );
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  Result := '';
  if InstalledVcRuntimeIsCurrent then
  begin
    Log('Skipping Microsoft Visual C++ Runtime installation.');
    Exit;
  end;
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
    Log('Microsoft Visual C++ Runtime installer could not be started.');
    Result := 'The Microsoft Visual C++ Runtime installer could not be started.';
  end
  else if ResultCode = 3010 then
  begin
    Log('Microsoft Visual C++ Runtime installer requested a restart.');
    NeedsRestart := True;
    Result := 'Restart Windows, then run Palmier Pro Setup again.';
  end
  else if ResultCode <> 0 then
  begin
    Log('Microsoft Visual C++ Runtime installer exit code ' + IntToStr(ResultCode) + '.');
    Result := 'The Microsoft Visual C++ Runtime installer failed with exit code '
      + IntToStr(ResultCode) + '.';
  end
  else
  begin
    Log('Microsoft Visual C++ Runtime installer completed successfully.');
  end;
end;
