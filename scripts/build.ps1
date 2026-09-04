param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [switch]$WithoutNif
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuildDirectory = Join-Path $ProjectRoot "build-msvc"
$ErlangDirectory = Join-Path $ProjectRoot "erlang"
$ErlangRoot = "C:\Program Files\Erlang OTP"

$CMakeArguments = @(
    "-S", $ProjectRoot,
    "-B", $BuildDirectory,
    "-DGAMEBATTLE_BUILD_TESTS=ON",
    "-DGAMEBATTLE_BUILD_CONFIG_COMPILER=OFF"
)

# A configured CMake directory already owns its generator and platform. Only
# select them on the first configure so this script can also reuse a directory
# created earlier by a plain `cmake -S . -B build-msvc` command.
if (-not (Test-Path -LiteralPath (Join-Path $BuildDirectory "CMakeCache.txt"))) {
    $CMakeArguments += "-G"
    $CMakeArguments += "Visual Studio 17 2022"
    $CMakeArguments += "-A"
    $CMakeArguments += "x64"
}

if (-not $WithoutNif) {
    $ErtsInclude = Get-ChildItem -LiteralPath $ErlangRoot -Directory -Filter "erts-*" |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "include" } |
        Where-Object { Test-Path -LiteralPath (Join-Path $_ "erl_nif.h") } |
        Select-Object -First 1
    if (-not $ErtsInclude) {
        throw "erl_nif.h was not found below $ErlangRoot. Use -WithoutNif or install Erlang/OTP."
    }
    $CMakeArguments += "-DGAMEBATTLE_BUILD_NIF=ON"
    $CMakeArguments += "-DERLANG_ERTS_INCLUDE_DIR=$ErtsInclude"
} else {
    $CMakeArguments += "-DGAMEBATTLE_BUILD_NIF=OFF"
}

& cmake @CMakeArguments
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

& cmake --build $BuildDirectory --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "C++ build failed" }

& ctest --test-dir $BuildDirectory -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "C++ tests failed" }

& cmake --install $BuildDirectory --config $Configuration --prefix $ErlangDirectory --component BattleRuntime
if ($LASTEXITCODE -ne 0) { throw "CMake install failed" }

$Rebar = Get-Command rebar3 -ErrorAction SilentlyContinue
if ($Rebar) {
    Push-Location $ErlangDirectory
    try {
        & $Rebar.Source compile
        if ($LASTEXITCODE -ne 0) { throw "Erlang compile failed" }
    } finally {
        Pop-Location
    }
} else {
    Write-Warning "rebar3 is not on PATH; C++ artifacts were installed, but Erlang modules were not compiled."
}
