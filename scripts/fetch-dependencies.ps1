param(
    [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"

if ($Platform -ne "x64") {
    throw "Only x64 is supported."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$depsRoot = Join-Path $repoRoot "deps"
$downloadsRoot = Join-Path $depsRoot "downloads"
$srcRoot = Join-Path $depsRoot "src"
$libRoot = Join-Path $depsRoot "lib"
$binRoot = Join-Path $depsRoot "bin"
$buildRoot = Join-Path $depsRoot "build"
$installedRoot = Join-Path $depsRoot "installed\x64-release"
$cmakeGenerator = "Visual Studio 17 2022"

function New-CleanDirectory {
    param([string]$Path)
    if (Test-Path $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Path | Out-Null
}

function Ensure-Directory {
    param([string]$Path)
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
}

function Get-ArchiveRoot {
    param([string]$Path)
    $dirs = Get-ChildItem -LiteralPath $Path -Directory
    if ($dirs.Count -eq 1) {
        return $dirs[0].FullName
    }
    return $Path
}

function Download-File {
    param(
        [string]$Url,
        [string]$OutFile
    )
    Write-Host "==> Downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $OutFile

    $prefixBytes = [System.IO.File]::ReadAllBytes($OutFile) | Select-Object -First 512
    $prefixText = [System.Text.Encoding]::ASCII.GetString($prefixBytes)
    if ($prefixText -match '<!doctype html' -or $prefixText -match '<html') {
        $html = Get-Content -LiteralPath $OutFile -Raw
        $metaRefreshMatch = [System.Text.RegularExpressions.Regex]::Match(
            $html,
            '<meta[^>]+http-equiv="refresh"[^>]+url=([^"]+)"',
            [System.Text.RegularExpressions.RegexOptions]::IgnoreCase
        )
        if ($metaRefreshMatch.Success) {
            $redirectUrl = [System.Net.WebUtility]::HtmlDecode($metaRefreshMatch.Groups[1].Value)
            Write-Host "==> Following SourceForge redirect to $redirectUrl"
            Invoke-WebRequest -Uri $redirectUrl -OutFile $OutFile
        }
    }
}

function Expand-Download {
    param(
        [string]$Name,
        [string]$Url,
        [ValidateSet("zip", "tar.gz")]
        [string]$ArchiveType = "zip"
    )
    $archiveExtension = if ($ArchiveType -eq "tar.gz") { "tar.gz" } else { "zip" }
    $archivePath = Join-Path $downloadsRoot "$Name.$archiveExtension"
    $extractPath = Join-Path $downloadsRoot $Name
    Download-File -Url $Url -OutFile $archivePath
    New-CleanDirectory -Path $extractPath
    if ($ArchiveType -eq "tar.gz") {
        & tar -xzf $archivePath -C $extractPath
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to extract $archivePath"
        }
    } else {
        Expand-Archive -LiteralPath $archivePath -DestinationPath $extractPath -Force
    }
    return (Get-ArchiveRoot -Path $extractPath)
}

function Copy-FirstMatch {
    param(
        [string]$SourceRoot,
        [string]$Filter,
        [string]$Destination
    )
    $match = Get-ChildItem -LiteralPath $SourceRoot -Recurse -File -Filter $Filter | Select-Object -First 1
    if (-not $match) {
        throw "Could not find $Filter under $SourceRoot"
    }
    Ensure-Directory -Path (Split-Path -Parent $Destination)
    Copy-Item -LiteralPath $match.FullName -Destination $Destination -Force
}

function Copy-BestLibMatch {
    param(
        [string]$SourceRoot,
        [string]$Filter,
        [string]$Destination
    )

    $matches = Get-ChildItem -LiteralPath $SourceRoot -Recurse -File -Filter $Filter
    if (-not $matches) {
        throw "Could not find $Filter under $SourceRoot"
    }

    $best = $matches |
        Sort-Object @{
            Expression = {
                $path = $_.FullName.ToLowerInvariant()
                $score = 0
                if ($path -match '\\x64\\') { $score += 100 }
                if ($path -match '\\release\\') { $score += 50 }
                if ($path -match '\\win32\\') { $score -= 100 }
                if ($path -match '\\debug\\') { $score -= 50 }
                if ($path -match 'macdll') { $score -= 25 }
                $score
            }
            Descending = $true
        }, @{
            Expression = { $_.FullName.Length }
            Descending = $false
        } |
        Select-Object -First 1

    Ensure-Directory -Path (Split-Path -Parent $Destination)
    Copy-Item -LiteralPath $best.FullName -Destination $Destination -Force
}

function Copy-BestLibMatchAny {
    param(
        [string]$SourceRoot,
        [string[]]$Filters,
        [string]$Destination
    )

    foreach ($filter in $Filters) {
        $matches = Get-ChildItem -LiteralPath $SourceRoot -Recurse -File -Filter $filter
        if ($matches) {
            Copy-BestLibMatch -SourceRoot $SourceRoot -Filter $filter -Destination $Destination
            return
        }
    }

    throw "Could not find any of the following under ${SourceRoot}: $($Filters -join ', ')"
}

function Copy-Files {
    param(
        [string]$SourceRoot,
        [string[]]$Files,
        [string]$DestinationRoot
    )
    Ensure-Directory -Path $DestinationRoot
    foreach ($file in $Files) {
        Copy-FirstMatch -SourceRoot $SourceRoot -Filter $file -Destination (Join-Path $DestinationRoot $file)
    }
}

function Get-VsInstallRoot {
    $vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $path = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
        if ($LASTEXITCODE -eq 0 -and $path) {
            return $path.Trim()
        }
    }

    $common7 = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Directory -Recurse -Filter Common7 -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
    if ($common7) {
        return (Split-Path -Parent $common7)
    }

    throw "Could not locate a Visual Studio installation."
}

function Initialize-VsDevEnvironment {
    $vsRoot = Get-VsInstallRoot
    $vsDevCmd = Join-Path $vsRoot "Common7\Tools\VsDevCmd.bat"
    if (-not (Test-Path -LiteralPath $vsDevCmd)) {
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
}

function Patch-MonkeysAudioCMake {
    param([string]$SourceRoot)

    $cmakeFile = Join-Path $SourceRoot "CMakeLists.txt"
    if (-not (Test-Path -LiteralPath $cmakeFile)) {
        return
    }

    $content = Get-Content -LiteralPath $cmakeFile -Raw
    $content = [System.Text.RegularExpressions.Regex]::Replace(
        $content,
        "add_compile_options\s*\(\s*-Wall\s+-Wextra\s*\)",
        "if(MSVC)`n  add_compile_options(/W4)`nelse()`n  add_compile_options(-Wall -Wextra)`nendif()",
        "SingleLine"
    )

    $content = [System.Text.RegularExpressions.Regex]::Replace(
        $content,
        "(?m)^[ \t]*Source/MACDll/MACDll\.cpp\r?\n",
        ""
    )
    Set-Content -LiteralPath $cmakeFile -Value $content -NoNewline
}

function Patch-OpusFileCMake {
    param([string]$SourceRoot)

    $cmakeFile = Join-Path $SourceRoot "CMakeLists.txt"
    if (-not (Test-Path -LiteralPath $cmakeFile)) {
        return
    }

    $content = Get-Content -LiteralPath $cmakeFile -Raw
    $content = [System.Text.RegularExpressions.Regex]::Replace(
        $content,
        "get_package_version\s*\(\s*PACKAGE_VERSION\s+PROJECT_VERSION\s*\)",
        "get_package_version(PACKAGE_VERSION PROJECT_VERSION)`nif(NOT PROJECT_VERSION MATCHES `"^[0-9]+\\.[0-9]+`")`n  set(PROJECT_VERSION `"0.1.0`")`nendif()",
        "SingleLine"
    )
    Set-Content -LiteralPath $cmakeFile -Value $content -NoNewline
}

function Invoke-CMakeBuild {
    param(
        [string]$Name,
        [string]$SourceDir,
        [string]$BuildDir,
        [string[]]$ConfigureArgs = @(),
        [switch]$Install
    )
    Ensure-Directory -Path $BuildDir
    & cmake --fresh -S $SourceDir -B $BuildDir -G $cmakeGenerator -A x64 -D "CMAKE_BUILD_TYPE=Release" @ConfigureArgs
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for $Name" }

    & cmake --build $BuildDir --config Release
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed for $Name" }

    if ($Install) {
        & cmake --install $BuildDir --config Release
        if ($LASTEXITCODE -ne 0) { throw "cmake install failed for $Name" }
    }
}

New-CleanDirectory -Path $downloadsRoot
Ensure-Directory -Path $srcRoot
Ensure-Directory -Path $libRoot
Ensure-Directory -Path $binRoot
Ensure-Directory -Path $buildRoot
Ensure-Directory -Path $installedRoot
Initialize-VsDevEnvironment

$apeUrl = if ($env:APE_SDK_URL) { $env:APE_SDK_URL } else { "https://monkeysaudio.com/files/MAC_1252_SDK.zip" }
$faad2Url = if ($env:FAAD2_URL) { $env:FAAD2_URL } else { "https://github.com/knik0/faad2/archive/refs/heads/master.zip" }
$alacUrl = if ($env:ALAC_URL) { $env:ALAC_URL } else { "https://github.com/macosforge/alac/archive/refs/heads/master.zip" }
$minimp4Url = if ($env:MINIMP4_URL) { $env:MINIMP4_URL } else { "https://raw.githubusercontent.com/lieff/minimp4/master/minimp4.h" }
$oggUrl = if ($env:OGG_URL) { $env:OGG_URL } else { "https://github.com/xiph/ogg/archive/refs/heads/master.zip" }
$opusUrl = if ($env:OPUS_URL) { $env:OPUS_URL } else { "https://github.com/xiph/opus/archive/refs/heads/master.zip" }
$opusfileUrl = if ($env:OPUSFILE_URL) { $env:OPUSFILE_URL } else { "https://github.com/xiph/opusfile/archive/refs/heads/master.zip" }
$srlaUrl = if ($env:SRLA_URL) { $env:SRLA_URL } else { "https://github.com/aikiriao/SRLA/archive/refs/heads/master.zip" }
$ttaUrl = if ($env:TTA_SOURCE_URL) { $env:TTA_SOURCE_URL } else { "https://downloads.sourceforge.net/project/tta/tta/libtta%2B%2B/libtta%2B%2B-2.1.tar.gz" }
$wavpackUrl = if ($env:WAVPACK_URL) { $env:WAVPACK_URL } else { "https://github.com/dbry/WavPack/archive/refs/heads/master.zip" }
$takUrl = $env:TAK_SDK_URL

Write-Host "==> Preparing Monkey's Audio SDK"
$apeRoot = Expand-Download -Name "ape" -Url $apeUrl
Patch-MonkeysAudioCMake -SourceRoot $apeRoot
Copy-Files -SourceRoot $apeRoot -Files @(
    "All.h",
    "MACLib.h",
    "Version.h",
    "Warnings.h",
    "WindowsEnvironment.h",
    "CharacterHelper.h",
    "SmartPtr.h"
) -DestinationRoot (Join-Path $srcRoot "ape")
$apeBuild = Join-Path $buildRoot "ape"
Invoke-CMakeBuild -Name "ape" -SourceDir $apeRoot -BuildDir $apeBuild -ConfigureArgs @(
    "-D", "BUILD_SHARED=OFF",
    "-D", "BUILD_UTIL=OFF"
)
Copy-BestLibMatch -SourceRoot $apeBuild -Filter "MAC.lib" -Destination (Join-Path $libRoot "ape\$Platform\Release\MACLib.lib")

Write-Host "==> Preparing FAAD2"
$faad2Root = Expand-Download -Name "faad2" -Url $faad2Url
Copy-FirstMatch -SourceRoot $faad2Root -Filter "neaacdec.h" -Destination (Join-Path $srcRoot "mp4\neaacdec.h")
$faad2Build = Join-Path $buildRoot "faad2"
Invoke-CMakeBuild -Name "faad2" -SourceDir $faad2Root -BuildDir $faad2Build -ConfigureArgs @(
    "-D", "BUILD_SHARED_LIBS=OFF"
)
Copy-BestLibMatch -SourceRoot $faad2Build -Filter "faad.lib" -Destination (Join-Path $libRoot "faad2\$Platform\Release\libfaad.lib")

Write-Host "==> Preparing ALAC and minimp4"
$alacRoot = Expand-Download -Name "alac" -Url $alacUrl
Copy-Files -SourceRoot $alacRoot -Files @(
    "ALACDecoder.h",
    "ALACDecoder.cpp",
    "ALACAudioTypes.h",
    "ALACBitUtilities.h",
    "ALACBitUtilities.c",
    "ag_dec.c",
    "dp_dec.c",
    "matrix_dec.c",
    "matrixlib.h",
    "EndianPortable.h",
    "EndianPortable.c",
    "dplib.h",
    "aglib.h"
) -DestinationRoot (Join-Path $srcRoot "mp4\alac")
Download-File -Url $minimp4Url -OutFile (Join-Path $srcRoot "mp4\minimp4.h")

Write-Host "==> Preparing ogg / opus / opusfile"
$oggRoot = Expand-Download -Name "ogg" -Url $oggUrl
$opusRoot = Expand-Download -Name "opus" -Url $opusUrl
$opusfileRoot = Expand-Download -Name "opusfile" -Url $opusfileUrl
Patch-OpusFileCMake -SourceRoot $opusfileRoot
Invoke-CMakeBuild -Name "ogg" -SourceDir $oggRoot -BuildDir (Join-Path $buildRoot "ogg") -ConfigureArgs @(
    "-D", "BUILD_SHARED_LIBS=OFF",
    "-D", "INSTALL_DOCS=OFF",
    "-D", "INSTALL_PKG_CONFIG_MODULE=OFF",
    "-D", "INSTALL_CMAKE_PACKAGE_MODULE=ON",
    "-D", "CMAKE_INSTALL_PREFIX=$installedRoot"
) -Install
Invoke-CMakeBuild -Name "opus" -SourceDir $opusRoot -BuildDir (Join-Path $buildRoot "opus") -ConfigureArgs @(
    "-D", "OPUS_BUILD_SHARED_LIBRARY=OFF",
    "-D", "OPUS_BUILD_PROGRAMS=OFF",
    "-D", "OPUS_BUILD_TESTING=OFF",
    "-D", "OPUS_INSTALL_PKG_CONFIG_MODULE=OFF",
    "-D", "OPUS_INSTALL_CMAKE_CONFIG_MODULE=ON",
    "-D", "OPUS_STATIC_RUNTIME=ON",
    "-D", "CMAKE_INSTALL_PREFIX=$installedRoot"
) -Install
Invoke-CMakeBuild -Name "opusfile" -SourceDir $opusfileRoot -BuildDir (Join-Path $buildRoot "opusfile") -ConfigureArgs @(
    "-D", "BUILD_SHARED_LIBS=OFF",
    "-D", "CMAKE_PREFIX_PATH=$installedRoot",
    "-D", "OP_DISABLE_HTTP=ON",
    "-D", "OP_DISABLE_EXAMPLES=ON",
    "-D", "OP_DISABLE_DOCS=ON",
    "-D", "CMAKE_INSTALL_PREFIX=$installedRoot"
) -Install

Write-Host "==> Preparing SRLA"
$srlaRoot = Expand-Download -Name "srla" -Url $srlaUrl
Copy-Files -SourceRoot $srlaRoot -Files @(
    "srla.h",
    "srla_decoder.h",
    "srla_encoder.h",
    "srla_stdint.h"
) -DestinationRoot (Join-Path $srcRoot "srla\include")
$srlaBuild = Join-Path $buildRoot "srla"
Invoke-CMakeBuild -Name "srla" -SourceDir $srlaRoot -BuildDir $srlaBuild -ConfigureArgs @(
    "-D", "without-test=ON"
)
Copy-BestLibMatch -SourceRoot $srlaBuild -Filter "srladec.lib" -Destination (Join-Path $libRoot "srla\$Platform\Release\srladec.lib")
Copy-BestLibMatch -SourceRoot $srlaBuild -Filter "srlacodec.lib" -Destination (Join-Path $libRoot "srla\$Platform\Release\srlacodec.lib")

Write-Host "==> Preparing WavPack"
$wvRoot = Expand-Download -Name "wavpack" -Url $wavpackUrl
Copy-FirstMatch -SourceRoot $wvRoot -Filter "wavpack.h" -Destination (Join-Path $srcRoot "wv\include\wavpack.h")
$wvBuild = Join-Path $buildRoot "wv"
Invoke-CMakeBuild -Name "wavpack" -SourceDir $wvRoot -BuildDir $wvBuild -ConfigureArgs @(
    "-D", "BUILD_SHARED_LIBS=OFF",
    "-D", "BUILD_TESTING=OFF",
    "-D", "WAVPACK_BUILD_PROGRAMS=OFF"
)
Copy-BestLibMatchAny -SourceRoot $wvBuild -Filters @("libwavpack.lib", "libwavpack.a") -Destination (Join-Path $libRoot "wv\$Platform\Release\libwavpack.lib")

Write-Host "==> Preparing libtta++"
$ttaRoot = Expand-Download -Name "tta" -Url $ttaUrl -ArchiveType "tar.gz"
Copy-Files -SourceRoot $ttaRoot -Files @(
    "libtta.h",
    "libtta.cpp",
    "filter.h",
    "config.h"
) -DestinationRoot (Join-Path $srcRoot "tta")

if ($takUrl) {
    Write-Host "==> Preparing TAK SDK"
    $takRoot = Expand-Download -Name "tak" -Url $takUrl
    Copy-FirstMatch -SourceRoot $takRoot -Filter "tak_deco_lib.h" -Destination (Join-Path $srcRoot "tak\tak_deco_lib.h")
    $takDll = Get-ChildItem -LiteralPath $takRoot -Recurse -File -Filter "tak_deco_lib.dll" | Select-Object -First 1
    if ($takDll) {
        Ensure-Directory -Path (Join-Path $binRoot "tak\$Platform\Release")
        Copy-Item -LiteralPath $takDll.FullName -Destination (Join-Path $binRoot "tak\$Platform\Release\tak_deco_lib.dll") -Force
    }
} else {
    Write-Warning "TAK_SDK_URL is not set. The tak configuration will remain unavailable until a direct SDK URL is provided."
}

Write-Host ""
Write-Host "Dependencies staged under: $depsRoot"
