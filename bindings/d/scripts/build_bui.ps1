# Build the C kit for the D binding. DUB runs this before compiling, so
# `dub build`, `dub run` and `dub test` work from a clean checkout.
#
# Set BUI_SOURCE_DIR to build another BUI checkout. Set BUI_CMAKE_GENERATOR
# to override the Visual Studio generator (for example "Ninja" with a
# matching compiler on PATH).
#
# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([string]$SourceDir = "")

$ErrorActionPreference = "Stop"

$package = Split-Path -Parent $PSScriptRoot
$root = if ($env:BUI_SOURCE_DIR) {
    $env:BUI_SOURCE_DIR
}
elseif ($SourceDir) {
    $SourceDir
}
else {
    (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
}
if (-not (Test-Path (Join-Path $root "kit/include/BUI_kit.h"))) {
    throw "BUI source tree not found at '$root' (set BUI_SOURCE_DIR)"
}
$root = (Resolve-Path $root).Path

$build = Join-Path $package ".bui/build"
$lib = Join-Path $package ".bui/lib"
New-Item -ItemType Directory -Force $build, $lib | Out-Null

function Find-CMake {
    if ($env:CMAKE -and (Test-Path $env:CMAKE)) {
        return $env:CMAKE
    }
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }
    return $null
}

function Find-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        return $null
    }
    $path = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null
    if (-not $path) {
        return $null
    }
    $version = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationVersion 2>$null
    $major = if ($version) { [int]($version.Split(".")[0]) } else { 0 }
    return [pscustomobject]@{
        Path = $path
        Generator = switch ($major) {
            18 { "Visual Studio 18 2026" }
            17 { "Visual Studio 17 2022" }
            16 { "Visual Studio 16 2019" }
            default { "Visual Studio 17 2022" }
        }
    }
}

$cmake = Find-CMake
$generator = $env:BUI_CMAKE_GENERATOR
$vs = Find-VisualStudio
if (-not $cmake -and $vs) {
    $candidate = Join-Path $vs.Path "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
    if (Test-Path $candidate) {
        $cmake = $candidate
    }
}
if (-not $generator -and $vs) {
    $generator = $vs.Generator
}
if (-not $cmake) {
    throw "cmake was not found (install CMake or the Visual Studio C++ tools)"
}
if (-not $generator) {
    $generator = "Visual Studio 17 2022"
}

# Configure only on the first run; CMake re-runs its checks after source edits.
if (-not (Test-Path (Join-Path $build "CMakeCache.txt"))) {
    $configure = @(
        "-S", $root, "-B", $build, "-G", $generator
    )
    if ($generator -like "Visual Studio*") {
        $configure += @("-A", "x64")
    }
    $configure += @(
        "-DBUI_BUILD_TESTS=OFF",
        "-DBUI_BUILD_EXAMPLES=OFF",
        "-DBUI_BUILD_PYTHON_BINDINGS=OFF",
        "-DBUI_BUILD_LUA_BINDINGS=OFF",
        "-DBUI_BUILD_QT_BACKEND=OFF",
        "-DBUI_BUILD_IMGUI=OFF"
    )
    & $cmake @configure
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed"
    }
}

& $cmake --build $build --config Release --target bui_kit bui_api
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed"
}

foreach ($name in @("kit", "gpu", "core", "api")) {
    $file = "bui_$name.lib"
    $source = Join-Path $build "$name/Release/$file"
    if (-not (Test-Path $source)) {
        $source = Join-Path $build "$name/$file"
    }
    if (-not (Test-Path $source)) {
        throw "built library not found: $source"
    }
    Copy-Item -Force $source (Join-Path $lib $file)
}
