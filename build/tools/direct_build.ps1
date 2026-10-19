# Sandbox cannot run cmake's try_compile, so call arm-none-eabi-gcc directly.
# Source list mirrors cmake/stm32cubemx/CMakeLists.txt.
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 as GBK,
# so non-ASCII comments here corrupt parsing.
# Params let you build a variant without touching any source file, e.g.
#   .\direct_build.ps1 -ExtraDefines @('APP_SERVO_TEST=1') -OutSub servotest
# Defaults (no args) build the car firmware (PS2 pad + mecanum chassis) in build\direct.
param(
    [string[]]$ExtraDefines = @(),
    [string]$OutSub = 'direct'
)
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root

# Toolchain lookup: ARM_GCC_BIN -> STM32Cube bundle -> PATH.
$gccBin = $null
if ($env:ARM_GCC_BIN) { $gccBin = $env:ARM_GCC_BIN }
if (-not $gccBin) {
    $bundles = Join-Path $env:LOCALAPPDATA 'stm32cube\bundles\gnu-tools-for-stm32'
    if (Test-Path $bundles) {
        $gccBin = Get-ChildItem -Path $bundles -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName 'bin' } |
            Where-Object { Test-Path (Join-Path $_ 'arm-none-eabi-gcc.exe') } |
            Select-Object -First 1
    }
}
if (-not $gccBin) {
    $onPath = Get-Command 'arm-none-eabi-gcc.exe' -ErrorAction SilentlyContinue
    if ($onPath) { $gccBin = Split-Path -Parent $onPath.Source }
}
if (-not $gccBin) { throw 'arm-none-eabi-gcc.exe not found: set ARM_GCC_BIN or put the toolchain on PATH.' }
$gcc    = Join-Path $gccBin 'arm-none-eabi-gcc.exe'
$size   = Join-Path $gccBin 'arm-none-eabi-size.exe'

$outDir = Join-Path $root ('build\' + $OutSub)
$objDir = Join-Path $outDir 'obj'
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

$inc = @(
    '-ICore/Inc'
    '-IDrivers/STM32F1xx_HAL_Driver/Inc'
    '-IDrivers/STM32F1xx_HAL_Driver/Inc/Legacy'
    '-IDrivers/CMSIS/Device/ST/STM32F1xx/Include'
    '-IDrivers/CMSIS/Include'
)
$cflags = @(
    '-mcpu=cortex-m3', '-mthumb', '-Wall', '-fdata-sections', '-ffunction-sections',
    '-fstack-usage', '-O0', '-g3', '-std=gnu11',
    '-DUSE_HAL_DRIVER', '-DSTM32F103xB'
) + ($ExtraDefines | ForEach-Object { '-D' + $_ }) + $inc

$appSrc = @(
    'Core/Src/main.c', 'Core/Src/stm32f1xx_it.c', 'Core/Src/stm32f1xx_hal_msp.c',
    'Core/Src/sysmem.c', 'Core/Src/syscalls.c'
)
$mySrc = @(
    'Core/Src/app.c', 'Core/Src/motor.c', 'Core/Src/chassis.c',
    'Core/Src/ps2.c', 'Core/Src/dwt_delay.c',
    'Core/Src/tune.c', 'Core/Src/arm.c'
)
$halSrc = (Get-ChildItem 'Drivers/STM32F1xx_HAL_Driver/Src/*.c' | ForEach-Object { $_.FullName }) + @('Core/Src/system_stm32f1xx.c')

$srcs = $appSrc + $mySrc + $halSrc
$objs = @()
$fail = 0
$warn = 0

foreach ($s in $srcs) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($s)
    $o = Join-Path $objDir ($name + '.o')
    $objs += $o
    $rsp = Join-Path $objDir ($name + '.rsp')
    # GCC's response-file parser treats '\' as an escape, so every path written
    # into an .rsp must use forward slashes.
    $lines = ($cflags + @('-c', $s, ('-o' + $o))) -replace '\\', '/'
    Set-Content -LiteralPath $rsp -Encoding ASCII -Value ($lines -join "`r`n")
    $log = & $gcc "@$rsp" 2>&1
    $txt = ($log | Out-String)
    if ($txt.Trim().Length -gt 0) {
        Write-Output "----- $s"
        Write-Output $txt.Trim()
        $warn += ([regex]::Matches($txt, 'warning:')).Count
    }
    if ($LASTEXITCODE -ne 0) { $fail++; Write-Output "FAILED: $s" }
}

# Startup file needs the assembler-with-cpp preprocessor pass.
$startupObj = Join-Path $objDir 'startup_stm32f103xb.o'
$rsp = Join-Path $objDir 'startup.rsp'
$sargs = @(
    '-mcpu=cortex-m3', '-mthumb', '-x', 'assembler-with-cpp',
    '-DUSE_HAL_DRIVER', '-DSTM32F103xB',
    '-c', 'startup_stm32f103xb.s', ('-o' + $startupObj)
)
Set-Content -LiteralPath $rsp -Encoding ASCII -Value (($sargs -replace '\\', '/') -join "`r`n")
$log = & $gcc "@$rsp" 2>&1
$txt = ($log | Out-String); if ($txt.Trim().Length -gt 0) { Write-Output $txt.Trim() }
if ($LASTEXITCODE -ne 0) { $fail++ }
$objs += $startupObj

Write-Output "=== compile failures: $fail   warnings: $warn ==="

$elf = Join-Path $outDir 'MyFirstCar.elf'
$map = Join-Path $outDir 'MyFirstCar.map'
$largs = @('-mcpu=cortex-m3', '-mthumb', '-T', 'STM32F103xx_FLASH.ld', '--specs=nano.specs') +
         @(('-Wl,-Map=' + $map), '-Wl,--gc-sections') + $objs + @('-o' + $elf, '-lm')
$lrsp = Join-Path $objDir 'link.rsp'
Set-Content -LiteralPath $lrsp -Encoding ASCII -Value (($largs -replace '\\', '/') -join "`r`n")
& $gcc "@$lrsp" 2>&1 | ForEach-Object { $_ }
Write-Output "=== link exit: $LASTEXITCODE ==="
if (Test-Path $elf) {
    & $size $elf
    Write-Output $elf
}
