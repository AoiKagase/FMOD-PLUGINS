param(
    [string]$Configuration = "Release",
    [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"

if ($Configuration -ne "Release") {
    throw "Only Release is supported by this script."
}
if ($Platform -ne "x64") {
    throw "Only x64 is supported by this script."
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$installPrefix = Join-Path $root "third_party\installed\x64-release"
$buildRoot = Join-Path $root "third_party\build"

$oggSource = (Resolve-Path (Join-Path $root "..\..\ogg")).Path
$opusSource = (Resolve-Path (Join-Path $root "..\..\opus")).Path
$opusfileSource = (Resolve-Path (Join-Path $root "..\..\opusfile")).Path

$vsDevCmd = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
$generator = "NMake Makefiles"
$runtime = "MultiThreaded"
$policy = "CMP0091=NEW"

if (-not (Test-Path $vsDevCmd)) {
    throw "VsDevCmd.bat was not found: $vsDevCmd"
}

$vsEnvironment = & cmd /c "`"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul && set"
if ($LASTEXITCODE -ne 0) {
    throw "Failed to initialize the Visual Studio developer environment."
}
foreach ($line in $vsEnvironment) {
    if ($line -match "^(.*?)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
    }
}

function Invoke-CmakeInstall {
    param(
        [string]$Name,
        [string]$SourceDir,
        [string]$BuildDir,
        [string[]]$ExtraArgs = @()
    )

    Write-Host "==> Configuring $Name"
    & cmake `
        --fresh `
        -S $SourceDir `
        -B $BuildDir `
        -G $generator `
        -D "CMAKE_BUILD_TYPE=$Configuration" `
        -D "CMAKE_INSTALL_PREFIX=$installPrefix" `
        -D "CMAKE_POLICY_DEFAULT_$policy" `
        -D "CMAKE_MSVC_RUNTIME_LIBRARY=$runtime" `
        @ExtraArgs
    if ($LASTEXITCODE -ne 0) {
        throw "cmake configure failed for $Name"
    }

    Write-Host "==> Building $Name"
    & cmake --build $BuildDir
    if ($LASTEXITCODE -ne 0) {
        throw "cmake build failed for $Name"
    }

    Write-Host "==> Installing $Name"
    & cmake --install $BuildDir
    if ($LASTEXITCODE -ne 0) {
        throw "cmake install failed for $Name"
    }
}

New-Item -ItemType Directory -Force -Path $installPrefix | Out-Null
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null

Invoke-CmakeInstall `
    -Name "ogg" `
    -SourceDir $oggSource `
    -BuildDir (Join-Path $buildRoot "ogg") `
    -ExtraArgs @(
        "-D", "BUILD_SHARED_LIBS=OFF",
        "-D", "INSTALL_DOCS=OFF",
        "-D", "INSTALL_PKG_CONFIG_MODULE=OFF",
        "-D", "INSTALL_CMAKE_PACKAGE_MODULE=ON"
    )

Invoke-CmakeInstall `
    -Name "opus" `
    -SourceDir $opusSource `
    -BuildDir (Join-Path $buildRoot "opus") `
    -ExtraArgs @(
        "-D", "OPUS_BUILD_SHARED_LIBRARY=OFF",
        "-D", "OPUS_BUILD_PROGRAMS=OFF",
        "-D", "OPUS_BUILD_TESTING=OFF",
        "-D", "OPUS_INSTALL_PKG_CONFIG_MODULE=OFF",
        "-D", "OPUS_INSTALL_CMAKE_CONFIG_MODULE=ON",
        "-D", "OPUS_STATIC_RUNTIME=ON"
    )

Invoke-CmakeInstall `
    -Name "opusfile" `
    -SourceDir $opusfileSource `
    -BuildDir (Join-Path $buildRoot "opusfile") `
    -ExtraArgs @(
        "-D", "BUILD_SHARED_LIBS=OFF",
        "-D", "CMAKE_PREFIX_PATH=$installPrefix",
        "-D", "OP_DISABLE_HTTP=ON",
        "-D", "OP_DISABLE_EXAMPLES=ON",
        "-D", "OP_DISABLE_DOCS=ON"
    )

Write-Host ""
Write-Host "Dependencies installed to: $installPrefix"
