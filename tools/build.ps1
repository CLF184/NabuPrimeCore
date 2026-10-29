param(
    [Parameter(Mandatory=$true)][string]$Zig,
    [Parameter(Mandatory=$true)][string]$WdkRoot,
    [Parameter(Mandatory=$true)][string]$SdkRoot,
    [string]$KitVersion = '10.0.26100.0',
    [string]$Python = 'py',
    [string]$OutDir,
    [switch]$RunTests
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $root 'build' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $OutDir 'zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $OutDir 'zig-local-cache'
$object = Join-Path $OutDir 'NabuPrime.obj'
$driver = Join-Path $OutDir 'NabuPrime.sys'
$includes = @('-isystem',(Join-Path $WdkRoot "Include\$KitVersion\km"),
    '-isystem',(Join-Path $WdkRoot "Include\$KitVersion\km\crt"),
    '-isystem',(Join-Path $SdkRoot "Include\$KitVersion\shared"))
& $Zig cc -x c++ -std=c++17 -target aarch64-windows-msvc -O2 `
    -ffreestanding -fno-builtin -fno-exceptions -fno-rtti -fno-stack-protector `
    -nostdinc -nostdlib -mgeneral-regs-only -D_ARM64_ -D_KERNEL_MODE `
    -DNTDDI_VERSION=0x0A000008 -D_WIN32_WINNT=0x0A00 @includes `
    -c (Join-Path $root 'drivers\nabu-prime\prime.cpp') -o $object
if ($LASTEXITCODE) { throw "ARM64 compilation failed: $LASTEXITCODE" }
& $Zig build-lib -dynamic $object `
    (Join-Path $WdkRoot "Lib\$KitVersion\km\ARM64\ntoskrnl.lib") `
    (Join-Path $WdkRoot "Lib\$KitVersion\km\ARM64\hal.lib") `
    -target aarch64-windows-msvc -fentry=DriverEntry --subsystem native `
    -fno-compiler-rt -fno-stack-check -fno-stack-protector -O ReleaseSmall `
    "-femit-bin=$driver" "-femit-implib=$(Join-Path $OutDir 'NabuPrime.lib')"
if ($LASTEXITCODE) { throw "ARM64 linking failed: $LASTEXITCODE" }
& $Python (Join-Path $root 'tests\validate_pe.py') $driver
if ($LASTEXITCODE) { throw "Driver image checks failed: $LASTEXITCODE" }
if ($RunTests) {
    $test = Join-Path $OutDir 'frequency-table-test.exe'
    & $Zig cc -x c++ -std=c++17 -target x86_64-windows-gnu -O2 -fno-exceptions -fno-rtti `
        (Join-Path $root 'tests\frequency_table_test.cpp') -o $test
    if ($LASTEXITCODE) { throw "Test compilation failed: $LASTEXITCODE" }
    & $test (Join-Path $root 'tests\fixtures\nabu-2841-lut.txt')
    if ($LASTEXITCODE) { throw "Frequency table tests failed: $LASTEXITCODE" }
}
Write-Output "Unsigned ARM64 driver: $driver"
