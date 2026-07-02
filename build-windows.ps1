# build-windows.ps1 — builds obs-dji-uvc against a local OBS checkout + obs-deps.
param(
    [string]$ObsDeps = "C:\obs-deps",
    [string]$ObsStudio = "",
    [string]$Config = "RelWithDebInfo"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

if (-not (Test-Path $ObsDeps)) {
    Write-Host "obs-deps not found at $ObsDeps."
    Write-Host "Get windows-deps-qt6-*-x64.zip from https://github.com/obsproject/obs-deps/releases"
    exit 1
}

if ($ObsStudio -eq "") {
    $ObsStudio = Join-Path $root "obs-studio"
}
if (-not (Test-Path $ObsStudio)) {
    git clone --depth 1 https://github.com/obsproject/obs-studio.git $ObsStudio
    git -C $ObsStudio submodule update --init --recursive --depth 1
}

$obsBuild = Join-Path $ObsStudio "build"
if (-not (Test-Path (Join-Path $obsBuild "libobs"))) {
    cmake -S $ObsStudio -B $obsBuild -G "Visual Studio 17 2022" -A x64 `
        -DCMAKE_PREFIX_PATH="$ObsDeps" `
        -DENABLE_PLUGINS=OFF -DENABLE_UI=OFF -DENABLE_SCRIPTING=OFF `
        -DENABLE_BROWSER=OFF -DENABLE_WEBSOCKET=OFF `
        -DCMAKE_INSTALL_PREFIX="$obsBuild\install"
}
cmake --build $obsBuild --config $Config --target libobs

# Try installing the cmake package; fall back to direct paths if unavailable.
cmake --install $obsBuild --config $Config --prefix "$obsBuild\install" 2>$null | Out-Null

$libobsConfig = Get-ChildItem -Path "$obsBuild\install" -Recurse -Filter "libobsConfig.cmake" -ErrorAction SilentlyContinue | Select-Object -First 1

$build = Join-Path $root "build"
$cmakeArgs = @(
    "-S", $root, "-B", $build,
    "-G", "Visual Studio 17 2022", "-A", "x64",
    "-DCMAKE_BUILD_TYPE=$Config"
)

if ($libobsConfig) {
    $cmakeArgs += "-DCMAKE_PREFIX_PATH=$ObsDeps;$obsBuild\install"
} else {
    Write-Host "libobsConfig.cmake not found — using direct paths into the build tree"
    $obsLib = Get-ChildItem -Path $obsBuild -Recurse -Filter "obs.lib" | Select-Object -First 1
    if (-not $obsLib) { throw "obs.lib not found under $obsBuild" }
    $pthreadsLib = Get-ChildItem -Path $obsBuild -Recurse -Filter "w32-pthreads.lib" -ErrorAction SilentlyContinue | Select-Object -First 1
    $cmakeArgs += "-DCMAKE_PREFIX_PATH=$ObsDeps"
    $cmakeArgs += "-DLIBOBS_INCLUDE_DIR=$ObsStudio\libobs"
    $cmakeArgs += "-DLIBOBS_LIB=$($obsLib.FullName)"
    if ($pthreadsLib) {
        $cmakeArgs += "-DLIBOBS_PTHREADS_LIB=$($pthreadsLib.FullName)"
        $cmakeArgs += "-DLIBOBS_PTHREADS_INCLUDE_DIR=$ObsStudio\deps\w32-pthreads"
    }
}

cmake @cmakeArgs
cmake --build $build --config $Config

Write-Host ""
Write-Host "Output: $build\$Config\obs-dji-uvc.dll"
