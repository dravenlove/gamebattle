param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuildDirectory = Join-Path $ProjectRoot "build-config-msvc"
$InstallDirectory = Join-Path $ProjectRoot "erlang"

$CMakeArguments = @(
    "-S", $ProjectRoot,
    "-B", $BuildDirectory,
    "-DGAMEBATTLE_BUILD_CONFIG_COMPILER=ON",
    "-DGAMEBATTLE_BUILD_TESTS=OFF",
    "-DGAMEBATTLE_BUILD_NIF=OFF"
)

if (-not (Test-Path -LiteralPath (Join-Path $BuildDirectory "CMakeCache.txt"))) {
    $CMakeArguments += "-G"
    $CMakeArguments += "Visual Studio 17 2022"
    $CMakeArguments += "-A"
    $CMakeArguments += "x64"
}

& cmake @CMakeArguments
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

& cmake --build $BuildDirectory --config $Configuration --target gamebattle_config_compiler
if ($LASTEXITCODE -ne 0) { throw "Config compiler build failed" }

& cmake --install $BuildDirectory --config $Configuration `
    --prefix $InstallDirectory --component ConfigTools
if ($LASTEXITCODE -ne 0) { throw "Config compiler install failed" }

Write-Host "Config compiler installed to $(Join-Path $InstallDirectory 'bin')"
