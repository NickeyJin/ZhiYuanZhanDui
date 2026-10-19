# flash.ps1
# Build (optional) and flash an STM32 ELF/HEX/BIN over SWD with STM32CubeProgrammer CLI.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File flash.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File flash.ps1 -Build:$true
#   powershell -NoProfile -ExecutionPolicy Bypass -File flash.ps1 -Elf build\direct\MyFirstCar.elf
#   powershell -NoProfile -ExecutionPolicy Bypass -File flash.ps1 -DryRun
#
# Default flow: flash <project>\build\direct\MyFirstCar.elf with verify and reset.
# With -Build:$true the script first runs build\tools\direct_build.ps1.
# -DryRun prints the exact command lines and changes nothing.
#
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 files using
# the system ANSI code page (GBK here), so non-ASCII text would corrupt parsing.
[CmdletBinding()]
param(
    [string]$Elf = '',
    [switch]$Build,
    [switch]$DryRun,
    [string]$SerialNumber = '',
    [int]$TimeoutSec = 120
)

$ErrorActionPreference = 'Stop'

$scriptDir = $PSScriptRoot
if ([string]::IsNullOrEmpty($scriptDir)) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
}
# scriptDir is <project>\build\tools
$projectRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)

if ([string]::IsNullOrEmpty($Elf)) {
    $Elf = Join-Path $projectRoot 'build\direct\MyFirstCar.elf'
} elseif (-not [System.IO.Path]::IsPathRooted($Elf)) {
    $Elf = Join-Path $projectRoot $Elf
}
$Elf = [System.IO.Path]::GetFullPath($Elf)

$bundleRoot = Join-Path $env:LOCALAPPDATA 'stm32cube\bundles'
$buildScript = Join-Path $scriptDir 'direct_build.ps1'
$psExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

function Find-BundleTool {
    param([string]$Bundle, [string]$Relative)
    $base = Join-Path $bundleRoot $Bundle
    if (-not (Test-Path -LiteralPath $base)) { return $null }
    $dirs = @(Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue |
              Sort-Object Name -Descending)
    foreach ($d in $dirs) {
        $candidate = Join-Path $d.FullName $Relative
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return $null
}

$progCli = Find-BundleTool 'programmer' 'bin\STM32_Programmer_CLI.exe'
if ($null -eq $progCli) {
    Write-Output ('ERROR: STM32_Programmer_CLI.exe not found under ' + $bundleRoot)
    exit 1
}

# ---------------------------------------------------------------------------
# Optional build
# ---------------------------------------------------------------------------
if ($Build) {
    if (-not (Test-Path -LiteralPath $buildScript)) {
        Write-Output ('ERROR: build script not found: ' + $buildScript)
        exit 1
    }
    $buildCmd = '"' + $psExe + '" -NoProfile -ExecutionPolicy Bypass -File "' + $buildScript + '"'
    if ($DryRun) {
        Write-Output ('BUILD: ' + $buildCmd)
    } else {
        Write-Output ('BUILD: ' + $buildCmd)
        $buildText = (& $psExe -NoProfile -ExecutionPolicy Bypass -File $buildScript 2>&1 | Out-String)
        Write-Output $buildText.TrimEnd()
        if ($buildText -notmatch 'compile failures: 0') {
            Write-Output 'ERROR: direct_build.ps1 reported compile failures.'
            exit 1
        }
        if ($buildText -notmatch 'link exit: 0') {
            Write-Output 'ERROR: direct_build.ps1 link step failed.'
            exit 1
        }
        if (-not (Test-Path -LiteralPath $Elf)) {
            Write-Output ('ERROR: ELF not produced: ' + $Elf)
            exit 1
        }
    }
}

if (-not (Test-Path -LiteralPath $Elf)) {
    Write-Output ('ERROR: ELF file not found: ' + $Elf)
    exit 1
}

# ---------------------------------------------------------------------------
# Build STM32_Programmer_CLI command line
#   -c port=SWD mode=UR   connect over SWD, connect under reset
#   -w <file>             write the file
#   -v                    verify after programming
#   -rst                  reset and run after programming
# ---------------------------------------------------------------------------
$cliArgs = '-c port=SWD mode=UR'
if ($SerialNumber -ne '') {
    $cliArgs += ' sn=' + $SerialNumber
}
$cliArgs += ' -w "' + $Elf + '" -v -rst'
$fullCmd = '"' + $progCli + '" ' + $cliArgs

if ($DryRun) {
    Write-Output ('FLASH: ' + $fullCmd)
    Write-Output 'DRY RUN: nothing was built or flashed.'
    exit 0
}

# ---------------------------------------------------------------------------
# Run the programmer with a hard timeout
# ---------------------------------------------------------------------------
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $progCli
$psi.Arguments = $cliArgs
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true
$psi.WorkingDirectory = $projectRoot

$proc = $null
$outText = ''
$errText = ''
$timedOut = $false

try {
    $proc = [System.Diagnostics.Process]::Start($psi)
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    if (-not $proc.WaitForExit([Math]::Max(10, $TimeoutSec) * 1000)) {
        $timedOut = $true
        try { $proc.Kill() } catch { }
    } else {
        $outText = $outTask.Result
        $errText = $errTask.Result
    }
} catch {
    Write-Output ('ERROR: failed to start STM32_Programmer_CLI: ' + $_.Exception.Message)
    exit 1
}

if ($timedOut) {
    Write-Output ('ERROR: STM32_Programmer_CLI timed out after ' + [Math]::Max(10, $TimeoutSec) + ' s; process killed.')
    Write-Output 'RESULT: FAILED'
    exit 1
}

Write-Output '--- STM32CubeProgrammer output ---'
if ($outText.Trim().Length -gt 0) { Write-Output $outText.TrimEnd() }
if ($errText.Trim().Length -gt 0) {
    Write-Output '--- stderr ---'
    Write-Output $errText.TrimEnd()
}
Write-Output ('--- exit code: ' + $proc.ExitCode + ' ---')

$failed = ($outText -match '(?im)^\s*(error|failure)') -or
          ($outText -match 'Unable to connect') -or
          ($outText -match 'No ST-LINK') -or
          ($errText -match '(?im)^\s*(error|failure)')

if (($proc.ExitCode -eq 0) -and (-not $failed)) {
    Write-Output 'RESULT: OK'
    exit 0
}

Write-Output 'RESULT: FAILED'
exit 1
