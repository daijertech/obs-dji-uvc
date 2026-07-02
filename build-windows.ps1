# build-windows.ps1 — builds obs-dji-uvc against a local OBS checkout + obs-deps.
#
# Prereqs:
#   * Visual Studio 2022 (Desktop C++ workload)
#   * CMake >= 3.22 in PATH
#   * git in PATH
#
# Usage:
#   .\build-windows.ps1 -ObsDeps C:\obs-deps -ObsStudio C:\obs-studio
#
# If -ObsStudio is omitted, the script clones and builds libobs headers only
# (frontend not required).

param(
    [string]$ObsDeps = "C:\obs-deps",
    [string]$ObsStudio = "",
    [string]$Config = "RelWithDebInfo"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

if (-not (Test-Path $ObsDeps)) {
    Write-Host "obs-deps not found at $ObsDeps."
    Write-Host "Download windows-deps from https://github.com/obsproject/obs-deps/releases"
    Write-Host "and extract so that $ObsDeps\include and $ObsDeps\lib exist."
    exit 1
}

if ($ObsStudio -eq "") {
    $ObsStudio = Join-Path $root "obs-studio"
    if (-not (Test-Path $ObsStudio)) {
        git clone --depth 1 https://github.com/obsproject/obs-studio.git $ObsStudio
        git -C $ObsStudio submodule update --init --recursive --depth 1 -- libobs deps
    }
    $obsBuild = Join-Path $ObsStudio "build"
    cmake -S $ObsStudio -B $obsBuild -G "Visual Studio 17 2022" -A x64 `
        -DCMAKE_PREFIX_PATH="$ObsDeps" `
        -DENABLE_PLUGINS=OFF -DENABLE_UI=OFF -DENABLE_SCRIPTING=OFF `
        -DCMAKE_INSTALL_PREFIX="$obsBuild\install"
    cmake --build $obsBuild --config $Config --target libobs
    cmake --install $obsBuild --config $Config --component obs_libraries 2>$null
}

$build = Join-Path $root "build"
cmake -S $root -B $build -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_PREFIX_PATH="$ObsDeps;$ObsStudio\build\install" `
    -DCMAKE_BUILD_TYPE=$Config
cmake --build $build --config $Config

Write-Host ""
Write-Host "Output: $build\$Config\obs-dji-uvc.dll"
Write-Host "Install: copy to C:\Program Files\obs-studio\obs-plugins\64bit\"
Write-Host "         copy data\ to C:\Program Files\obs-studio\data\obs-plugins\obs-dji-uvc\"
Write-Host "Also copy libusb-1.0.dll next to the plugin DLL if built shared."
