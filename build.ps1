# Build compiler treelang (Release) memakai CMake bawaan Visual Studio.
# Pemakaian:  .\build.ps1          build
#             .\build.ps1 -Test    build lalu jalankan semua test
param([switch]$Test)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# File sementara build/test di dalam proyek (bukan %TEMP% di drive sistem).
$tmp = Join-Path $PSScriptRoot "build\tmp"
New-Item -ItemType Directory -Force $tmp | Out-Null
$env:TEMP = $tmp
$env:TMP = $tmp

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -property installationPath
    $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}

if (-not (Test-Path "build\CMakeCache.txt")) {
    & $cmake -B build -A x64
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
& $cmake --build build --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Test) {
    python test\run_tests.py build\Release\treelang.exe
    $rc = $LASTEXITCODE
    python test\http_tests.py build\Release\treelang.exe --quick
    if ($LASTEXITCODE -ne 0) { $rc = $LASTEXITCODE }
    exit $rc
}
