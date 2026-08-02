[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$InstallerPath,
    [Parameter(Mandatory = $true)][string]$WorkRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$installer = (Resolve-Path -LiteralPath $InstallerPath).Path
$work = [System.IO.Path]::GetFullPath($WorkRoot)
if (Test-Path -LiteralPath $work) {
    if ((Get-ChildItem -LiteralPath $work -Force | Select-Object -First 1)) {
        throw "Installer test root must be absent or empty: $work"
    }
} else {
    New-Item -ItemType Directory -Path $work | Out-Null
}
$installRoot = Join-Path $work 'installed'
$userProject = Join-Path $work 'user-project.palmier'
New-Item -ItemType Directory -Path $userProject | Out-Null
$sentinel = Join-Path $userProject 'preserve-me.txt'
[System.IO.File]::WriteAllText($sentinel, 'user-project-data', [System.Text.UTF8Encoding]::new($false))
$setupLog = Join-Path $work 'setup.log'

$setup = Start-Process -FilePath $installer -ArgumentList @(
    '/VERYSILENT',
    '/SUPPRESSMSGBOXES',
    '/NORESTART',
    '/SP-',
    "/LOG=$setupLog",
    "/DIR=$installRoot"
) -Wait -PassThru
if ($setup.ExitCode -ne 0) {
    if (Test-Path -LiteralPath $setupLog -PathType Leaf) {
        Get-Content -LiteralPath $setupLog -Tail 100 -Encoding UTF8 | Write-Output
    }
    throw "Prototype installer failed with exit code $($setup.ExitCode)"
}
$setupLogText = Get-Content -LiteralPath $setupLog -Raw -Encoding UTF8
$skippedRuntime = $setupLogText.Contains('current=True') -and
    $setupLogText.Contains('Skipping Microsoft Visual C++ Runtime installation.')
$installedRuntime = $setupLogText.Contains(
    'Microsoft Visual C++ Runtime installer completed successfully.'
)
if (-not $skippedRuntime -and -not $installedRuntime) {
    throw 'Prototype setup log did not prove the Visual C++ Runtime prerequisite path.'
}

$application = Join-Path $installRoot 'PalmierPro.exe'
if (-not (Test-Path -LiteralPath $application -PathType Leaf)) {
    throw "Installed application is missing: $application"
}
$previousPath = $env:PATH
$previousPlatform = $env:QT_QPA_PLATFORM
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:QT_QPA_PLATFORM = 'windows'
    $smoke = Start-Process -FilePath $application -ArgumentList '--smoke-test' -WorkingDirectory $installRoot -Wait -PassThru
    if ($smoke.ExitCode -ne 0) { throw "Installed application smoke failed with exit code $($smoke.ExitCode)" }
} finally {
    $env:PATH = $previousPath
    $env:QT_QPA_PLATFORM = $previousPlatform
}

$uninstaller = Join-Path $installRoot 'unins000.exe'
if (-not (Test-Path -LiteralPath $uninstaller -PathType Leaf)) {
    throw "Prototype uninstaller is missing: $uninstaller"
}
$uninstall = Start-Process -FilePath $uninstaller -ArgumentList @(
    '/VERYSILENT',
    '/SUPPRESSMSGBOXES',
    '/NORESTART'
) -Wait -PassThru
if ($uninstall.ExitCode -ne 0) { throw "Prototype uninstaller failed with exit code $($uninstall.ExitCode)" }
if (Test-Path -LiteralPath $application) { throw 'Uninstall left the application executable behind.' }
if (-not (Test-Path -LiteralPath $sentinel -PathType Leaf)) { throw 'Uninstall removed external user project data.' }
if ((Get-Content -LiteralPath $sentinel -Raw -Encoding UTF8) -ne 'user-project-data') {
    throw 'Uninstall changed external user project data.'
}

Write-Output 'Prototype install, isolated launch, uninstall, and user-data preservation passed.'
