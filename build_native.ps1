$ErrorActionPreference = 'Stop'

$projectDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$compilerCommand = Get-Command g++.exe -ErrorAction SilentlyContinue
if (-not $compilerCommand) {
    throw 'g++.exe was not found. Install a MinGW-w64 UCRT toolchain and try again.'
}

$source = Join-Path $projectDirectory 'main.cpp'
$target = Join-Path $projectDirectory 'Nobu16DlcBookExtractor.exe'

& $compilerCommand.Source `
    -std=c++17 -Os -s -flto -fno-rtti -ffunction-sections -fdata-sections `
    -static -static-libgcc -static-libstdc++ `
    -municode -mwindows -DUNICODE -D_UNICODE `
    '-Wl,--gc-sections' `
    $source `
    -lole32 -luuid -lwindowscodecs -lcomctl32 -lshell32 `
    -o $target

if ($LASTEXITCODE -ne 0) {
    throw "Native build failed with exit code $LASTEXITCODE."
}

Write-Host "Built: $target"
