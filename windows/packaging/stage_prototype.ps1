[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RepositoryRoot,
    [Parameter(Mandatory = $true)][string]$BuildRoot,
    [Parameter(Mandatory = $true)][string]$QtRoot,
    [Parameter(Mandatory = $true)][string]$QtLicenseRoot,
    [Parameter(Mandatory = $true)][string]$VcRedistPath,
    [Parameter(Mandatory = $true)][string]$StageRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$script:AppLocalDllNames = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase
)

function Resolve-RequiredPath([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Label does not exist: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Get-SourceLabel([string]$RelativePath) {
    $normalized = $RelativePath.Replace('\', '/')
    if ($normalized -eq 'PalmierPro.exe') { return 'repository-build' }
    if ($normalized -like 'redist/*') { return 'msvc-redist' }
    if ($normalized -like 'licenses/ffmpeg/*' -or $normalized -match '(^|/)(avcodec|avformat|avutil|swresample|swscale)-.*\.dll$') { return 'vcpkg-ffmpeg-8.1.2' }
    if ($script:AppLocalDllNames.Contains($normalized)) { return 'vcpkg-app-local' }
    if ($normalized -like 'licenses/qt/*' -or $normalized -like 'Qt6*.dll' -or $normalized -like 'qml/*' -or $normalized -like 'plugins/*' -or $normalized -like 'platforms/*') { return 'qt-6.10.3' }
    if ($normalized -match '^(imageformats|iconengines|styles|networkinformation|tls|generic|sqldrivers|accessible|bearer)/' -or $normalized -in @('qt.conf', 'd3dcompiler_47.dll', 'opengl32sw.dll', 'dxcompiler.dll', 'dxil.dll') -or $normalized -match '^icu.*\.dll$') { return 'qt-deployment-support' }
    if ($normalized -like 'licenses/*' -or $normalized -like '*.txt') { return 'repository-notice' }
    if ($normalized -match '\.(dll|exe)$') { throw "Unclassified runtime binary: $normalized" }
    return 'deployment-runtime'
}

$repository = Resolve-RequiredPath $RepositoryRoot 'Repository root'
$build = Resolve-RequiredPath $BuildRoot 'Build root'
$qt = Resolve-RequiredPath $QtRoot 'Qt root'
$qtLicenseDirectory = Resolve-RequiredPath $QtLicenseRoot 'Qt license directory'
$vcRedist = Resolve-RequiredPath $VcRedistPath 'Visual C++ redistributable'
$stage = [System.IO.Path]::GetFullPath($StageRoot)
if (Test-Path -LiteralPath $stage) {
    if ((Get-ChildItem -LiteralPath $stage -Force | Select-Object -First 1)) {
        throw "Stage root must be absent or empty: $stage"
    }
} else {
    New-Item -ItemType Directory -Path $stage | Out-Null
}

$toolchain = Get-Content -LiteralPath (Join-Path $repository 'windows\toolchain.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($toolchain.prototypeDependencies[0].name -ne 'Qt' -or $toolchain.prototypeDependencies[0].version -ne '6.10.3') {
    throw 'Qt staging requires the locked 6.10.3 toolchain entry.'
}
if ($toolchain.prototypeDependencies[1].name -ne 'FFmpeg' -or $toolchain.prototypeDependencies[1].version -ne '8.1.2') {
    throw 'FFmpeg staging requires the locked 8.1.2 toolchain entry.'
}

$builtApplication = Resolve-RequiredPath (Join-Path $build 'windows\app\Release\palmier_qt_shell.exe') 'Qt application'
$applicationDirectory = Split-Path -Parent $builtApplication
& cmake --install $build --config Release --prefix $stage --component prototype
if ($LASTEXITCODE -ne 0) { throw "CMake prototype install failed with exit code $LASTEXITCODE" }
$installedApplication = Resolve-RequiredPath (Join-Path $stage 'palmier_qt_shell.exe') 'Installed Qt application'
Move-Item -LiteralPath $installedApplication -Destination (Join-Path $stage 'PalmierPro.exe')
Get-ChildItem -LiteralPath $applicationDirectory -File -Filter '*.dll' | ForEach-Object {
    $script:AppLocalDllNames.Add($_.Name) | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $stage $_.Name)
}

$deployTool = Resolve-RequiredPath (Join-Path $qt 'bin\windeployqt.exe') 'windeployqt'
& $deployTool --release --no-compiler-runtime --no-translations --skip-plugin-types qmltooling,generic --qmldir (Join-Path $repository 'windows\app\qml') --dir $stage (Join-Path $stage 'PalmierPro.exe')
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE" }

$requiredRuntime = @(
    'PalmierPro.exe',
    'Qt6Core.dll',
    'platforms\qwindows.dll'
)
foreach ($relativePath in $requiredRuntime) {
    Resolve-RequiredPath (Join-Path $stage $relativePath) "Deployed runtime $relativePath" | Out-Null
}
foreach ($pattern in @('avcodec-*.dll', 'avformat-*.dll', 'avutil-*.dll', 'swresample-*.dll', 'swscale-*.dll')) {
    if (-not (Get-ChildItem -LiteralPath $stage -File -Filter $pattern | Select-Object -First 1)) {
        throw "Deployed FFmpeg runtime is missing $pattern"
    }
}

$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Path $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $repository 'LICENSE') -Destination (Join-Path $licenses 'PalmierPro-GPLv3.txt')
Copy-Item -LiteralPath (Join-Path $repository 'windows\packaging\THIRD_PARTY_NOTICES.prototype.txt') -Destination (Join-Path $stage 'THIRD_PARTY_NOTICES.txt')

$qtLgpl = Join-Path $qtLicenseDirectory 'LGPL-3.0-only.txt'
Resolve-RequiredPath $qtLgpl 'Qt LGPL-3.0 license text' | Out-Null
Copy-Item -LiteralPath $qtLicenseDirectory -Destination (Join-Path $licenses 'qt') -Recurse
$ffmpegCopyright = Resolve-RequiredPath (Join-Path $build 'vcpkg_installed\x64-windows\share\ffmpeg\copyright') 'vcpkg FFmpeg copyright record'
$ffmpegLicenseDirectory = Join-Path $licenses 'ffmpeg'
New-Item -ItemType Directory -Path $ffmpegLicenseDirectory | Out-Null
Copy-Item -LiteralPath $ffmpegCopyright -Destination (Join-Path $ffmpegLicenseDirectory 'copyright.txt')

$redistDirectory = Join-Path $stage 'redist'
New-Item -ItemType Directory -Path $redistDirectory | Out-Null
Copy-Item -LiteralPath $vcRedist -Destination (Join-Path $redistDirectory 'vc_redist.x64.exe')

$probe = Resolve-RequiredPath (Join-Path $build 'windows\packaging\Release\palmier_ffmpeg_distribution_probe.exe') 'FFmpeg distribution probe'
$evidencePath = Join-Path $stage 'FFMPEG_RUNTIME_EVIDENCE.txt'
& $probe 2>&1 | Out-File -LiteralPath $evidencePath -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "FFmpeg distribution probe failed with exit code $LASTEXITCODE" }

$manifestPath = Join-Path $stage 'RUNTIME_MANIFEST.json'
$files = Get-ChildItem -LiteralPath $stage -File -Recurse |
    Where-Object { $_.FullName -ne $manifestPath } |
    Sort-Object -Property FullName |
    ForEach-Object {
        $relative = [System.IO.Path]::GetRelativePath($stage, $_.FullName).Replace('\', '/')
        [ordered]@{
            path = $relative
            size = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            source = Get-SourceLabel $relative
        }
    }
$manifest = [ordered]@{
    schemaVersion = 1
    product = 'Palmier Pro Windows Prototype'
    productVersion = '0.6.16'
    target = [ordered]@{
        minimumOs = $toolchain.target.minimumOs
        minimumBuild = $toolchain.target.minimumBuild
        architecture = $toolchain.target.architecture
    }
    unsigned = $true
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    files = @($files)
}
$manifestJson = $manifest | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText($manifestPath, $manifestJson + "`n", [System.Text.UTF8Encoding]::new($false))

Write-Output "Staged $($manifest.files.Count) runtime files at $stage"
