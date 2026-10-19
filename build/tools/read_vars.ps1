# read_vars.ps1
# Read live C variables from an STM32 target through ST-LINK GDB server + arm-none-eabi-gdb.
# READ-ONLY: this script never flashes, erases, or writes target memory.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File read_vars.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File read_vars.ps1 -Symbols ps2_ok,ps2_lx
#   powershell -NoProfile -ExecutionPolicy Bypass -File read_vars.ps1 -Elf build\direct\MyFirstCar.elf
#
# Output: one "name = value" line per requested symbol.
# Exit code: 0 if every symbol was read, 1 otherwise.
#
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 files using
# the system ANSI code page (GBK here), so non-ASCII text would corrupt parsing.
[CmdletBinding()]
param(
    [string]$Symbols = 'ps2_ok,ps2_btn1,ps2_btn2,ps2_lx,ps2_ly,ps2_rx,ps2_ry',
    [string]$Elf = '',
    [int]$Port = 61234,
    [int]$TimeoutSec = 40,
    [string]$STLinkSN = ''
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
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

$runDir     = Join-Path $scriptDir '_read_vars_tmp'
$gsLog      = Join-Path $runDir 'gdbserver.log'
$gsOut      = Join-Path $runDir 'gdbserver.out'
$gdbOut     = Join-Path $runDir 'gdb.out'
$gdbErr     = Join-Path $runDir 'gdb.err'
$gdbSetup   = Join-Path $runDir 'gdb_setup.cmd'

$fatal      = $null
$serverProc = $null
$gdbProc    = $null
$outLines   = New-Object System.Collections.Generic.List[string]
$exitCode   = 1

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
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

function Stop-ProcessTree {
    param($Proc)
    if ($null -eq $Proc) { return }
    try {
        if (-not $Proc.HasExited) {
            $taskkill = Join-Path $env:SystemRoot 'System32\taskkill.exe'
            if (Test-Path -LiteralPath $taskkill) {
                & $taskkill /PID $Proc.Id /T /F 2>&1 | Out-Null
            } else {
                $Proc.Kill()
            }
        }
    } catch {
        # Best effort only.
    }
}

function Stop-AllGdbServers {
    # One ST-LINK probe can only be owned by one GDB server. A stale server from
    # an earlier run would keep our port busy, so always clear leftovers.
    $procs = @(Get-Process -Name 'ST-LINK_gdbserver' -ErrorAction SilentlyContinue)
    foreach ($p in $procs) {
        try { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } catch { }
    }
}

function Read-TextFile {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    try {
        # Get-Content opens with sharing that tolerates the writer. The .NET
        # File.ReadAllText call fails while gdbserver holds its log file open.
        $text = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
        if ($null -eq $text) { return '' }
        return [string]$text
    } catch {
        return ''
    }
}

function Get-MarkerBlock {
    param([string]$Text, [string]$StartMarker, [string]$EndMarker)
    if ([string]::IsNullOrEmpty($Text)) { return '' }
    $pattern = [regex]::Escape($StartMarker) + '(.*?)' + [regex]::Escape($EndMarker)
    $m = [regex]::Match($Text, $pattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)
    if ($m.Success) { return $m.Groups[1].Value }
    return ''
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
try {
    if (-not (Test-Path -LiteralPath $Elf)) {
        throw ('ELF file not found: ' + $Elf)
    }

    # Parse and validate the symbol list.
    $symList = New-Object System.Collections.Generic.List[string]
    foreach ($piece in ($Symbols -split ',')) {
        $name = $piece.Trim()
        if ($name.Length -eq 0) { continue }
        if ($name -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') {
            throw ('Invalid C symbol name: "' + $name + '"')
        }
        if (-not $symList.Contains($name)) { $symList.Add($name) }
    }
    if ($symList.Count -eq 0) {
        throw 'No symbols requested.'
    }

    $gdb       = Find-BundleTool 'gnu-gdb-for-stm32' 'bin\arm-none-eabi-gdb.exe'
    $gdbServer = Find-BundleTool 'stlink-gdbserver'  'bin\ST-LINK_gdbserver.exe'
    $progCli   = Find-BundleTool 'programmer'        'bin\STM32_Programmer_CLI.exe'
    $readElf   = Find-BundleTool 'gnu-tools-for-stm32' 'bin\arm-none-eabi-readelf.exe'

    if ($null -eq $gdb)       { throw ('arm-none-eabi-gdb.exe not found under ' + $bundleRoot) }
    if ($null -eq $gdbServer) { throw ('ST-LINK_gdbserver.exe not found under ' + $bundleRoot) }
    if ($null -eq $progCli)   { throw ('STM32_Programmer_CLI.exe not found under ' + $bundleRoot) }
    $progBin = Split-Path -Parent $progCli

    if (-not (Test-Path -LiteralPath $runDir)) {
        New-Item -ItemType Directory -Path $runDir -Force | Out-Null
    }

    # Clear stale debug servers before starting a fresh one, then remove their
    # logs so a stale "Waiting for connection" line cannot fake readiness.
    Stop-AllGdbServers
    Start-Sleep -Milliseconds 300
    foreach ($f in @($gsLog, $gsOut, $gdbOut, $gdbErr, $gdbSetup)) {
        Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue
    }

    # -----------------------------------------------------------------------
    # Start ST-LINK GDB server
    # -----------------------------------------------------------------------
    $gsArgs = '-p ' + $Port + ' -m 0 -d -l 31 -f "' + $gsLog + '" -cp "' + $progBin + '"'
    if ($STLinkSN -ne '') {
        $gsArgs += ' -i ' + $STLinkSN
    }
    $cmdExe = Join-Path $env:SystemRoot 'System32\cmd.exe'
    $gsCmdLine = '/c ""' + $gdbServer + '" ' + $gsArgs + ' > "' + $gsOut + '" 2>&1"'

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $cmdExe
    $psi.Arguments = $gsCmdLine
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.WorkingDirectory = $runDir
    $serverProc = [System.Diagnostics.Process]::Start($psi)

    $ready = $false
    $deadline = (Get-Date).AddSeconds([Math]::Max(5, $TimeoutSec))
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 250
        $logText = Read-TextFile $gsLog
        if ($logText -match ('Waiting for connection on port ' + [string]$Port)) {
            $ready = $true
            break
        }
        if ($serverProc.HasExited) {
            break
        }
    }

    if (-not $ready) {
        $detail = Read-TextFile $gsOut
        $detail = (@($detail -split "`r?`n") | Where-Object { $_.Trim().Length -gt 0 } | Select-Object -Last 3) -join ' | '
        if ($detail.Length -gt 0) {
            throw ('ST-LINK GDB server did not start on port ' + $Port + '. ' + $detail + ' (logs: ' + $runDir + ')')
        }
        throw ('ST-LINK GDB server did not start on port ' + $Port + ' (logs: ' + $runDir + ')')
    }

    # Small settle delay so the listening socket is definitely up.
    Start-Sleep -Milliseconds 300

    # -----------------------------------------------------------------------
    # GDB setup commands: load symbols, connect, read target PC and flash vector
    # -----------------------------------------------------------------------
    $elfGdb = $Elf -replace '\\', '/'
    $setup = @(
        'set pagination off',
        'set confirm off',
        'set width 0',
        'set print elements 0',
        ('file "' + $elfGdb + '"'),
        ('target extended-remote 127.0.0.1:' + $Port),
        'echo ===TARGET===\n',
        'info registers pc',
        'echo ===FLASH===\n',
        'x/2wx 0x08000000',
        'echo ===END===\n'
    )
    Set-Content -LiteralPath $gdbSetup -Encoding ASCII -Value $setup

    $exParts = New-Object System.Collections.Generic.List[string]
    foreach ($name in $symList) {
        $exParts.Add('-ex "echo ===SYMBOL:' + $name + '===\n"')
        $exParts.Add('-ex "p/x ' + $name + '"')
    }
    $exParts.Add('-ex "detach"')
    $exParts.Add('-ex "quit"')

    $gdbCmdLine = '/c ""' + $gdb + '" --batch -nx -x "' + $gdbSetup + '" ' +
                  ($exParts -join ' ') + ' > "' + $gdbOut + '" 2> "' + $gdbErr + '""'

    $gpsi = New-Object System.Diagnostics.ProcessStartInfo
    $gpsi.FileName = $cmdExe
    $gpsi.Arguments = $gdbCmdLine
    $gpsi.UseShellExecute = $false
    $gpsi.CreateNoWindow = $true
    $gpsi.WorkingDirectory = $runDir
    $gdbProc = [System.Diagnostics.Process]::Start($gpsi)

    if (-not $gdbProc.WaitForExit([Math]::Max(10, $TimeoutSec) * 1000)) {
        Stop-ProcessTree $gdbProc
        throw ('GDB timed out after ' + [Math]::Max(10, $TimeoutSec) + ' s (logs: ' + $runDir + ')')
    }

    $gdbText = Read-TextFile $gdbOut
    $gdbErrText = Read-TextFile $gdbErr

    # A failed "target extended-remote" still lets "p/x" fall back to values from
    # the ELF file, which would print bogus data. Require a real PC read first.
    $targetBlock = Get-MarkerBlock $gdbText '===TARGET===' '===FLASH==='
    if ($targetBlock -notmatch '(?m)^pc\s+0x[0-9a-fA-F]+') {
        $hint = (@($gdbErrText -split "`r?`n") | Where-Object { $_.Trim().Length -gt 0 } | Select-Object -Last 2) -join ' | '
        if ($hint.Length -eq 0) {
            $hint = 'no target response'
        }
        throw ('GDB could not attach to the target at 127.0.0.1:' + $Port + '. ' + $hint + ' (logs: ' + $runDir + ')')
    }

    # -----------------------------------------------------------------------
    # Warn when the ELF does not match the image currently in flash
    # -----------------------------------------------------------------------
    $flashBlock = Get-MarkerBlock $gdbText '===FLASH===' '===END==='
    $fm = [regex]::Match($flashBlock, '0x0*8000000:\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)')
    if ($fm.Success) {
        $flashReset = [Convert]::ToUInt64($fm.Groups[2].Value.Substring(2), 16)
        $entry = $null
        if ($null -ne $readElf) {
            $readElfOut = & $readElf -h $Elf 2>$null
            foreach ($line in $readElfOut) {
                if ($line -match 'Entry point address:\s*(0x[0-9a-fA-F]+)') {
                    $entry = [Convert]::ToUInt64($Matches[1].Substring(2), 16)
                    break
                }
            }
        }
        if (($null -ne $entry) -and (($entry -band 0xFFFFFFFE) -ne ($flashReset -band 0xFFFFFFFE))) {
            $msg = 'ELF entry 0x{0:X8} does not match the flash reset vector 0x{1:X8} at 0x08000004. ' -f $entry, $flashReset
            $msg += 'The board may be running different firmware; flash this ELF before trusting symbol values.'
            Write-Warning $msg
        }
    }

    # -----------------------------------------------------------------------
    # Parse "name = value" results
    # -----------------------------------------------------------------------
    $values = @{}
    $failures = @{}
    foreach ($name in $symList) {
        $pattern = '===SYMBOL:' + [regex]::Escape($name) + '===\s*\r?\n(.*?)(?====SYMBOL:|\z)'
        $m = [regex]::Match($gdbText, $pattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)
        $body = ''
        if ($m.Success) { $body = $m.Groups[1].Value }
        $vm = [regex]::Match($body, '(?m)^\$\d+\s*=\s*(.+?)\s*$')
        if ($vm.Success) {
            $values[$name] = $vm.Groups[1].Value
        } else {
            $values[$name] = $null
            $reason = 'not readable'
            if ($gdbErrText -match ('No symbol "' + [regex]::Escape($name) + '"')) {
                $reason = 'no such symbol'
            } elseif ($body -match 'optimized out') {
                $reason = 'optimized out'
            } elseif ($body.Trim().Length -gt 0) {
                $reason = $body.Trim()
            }
            $failures[$name] = $reason
        }
    }

    $allOk = $true
    foreach ($name in $symList) {
        if ($null -ne $values[$name]) {
            $outLines.Add($name + ' = ' + $values[$name])
        } else {
            $outLines.Add($name + ' = <error: ' + $failures[$name] + '>')
            $allOk = $false
        }
    }

    if ($allOk) { $exitCode = 0 } else { $exitCode = 1 }
} catch {
    $fatal = $_.Exception.Message
} finally {
    Stop-ProcessTree $gdbProc
    Stop-ProcessTree $serverProc
    Stop-AllGdbServers
}

if ($null -ne $fatal) {
    Write-Output ('ERROR: ' + $fatal)
    exit 1
}

foreach ($line in $outLines) {
    Write-Output $line
}
exit $exitCode
