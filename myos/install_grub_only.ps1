# install_grub_only.ps1 - GRUB on USB boot partition only (no repartition)
# Run as Administrator. Pen drive must show as X: in Explorer.

$ErrorActionPreference = "Continue"

if (-NOT (Test-Path "X:\boot")) {
    Write-Host "[ERROR] X:\boot not found. Plug in the pen drive and run flash first." -ForegroundColor Red
    exit 1
}

function Get-WslPath {
    param([string]$WinPath)
    $full = [System.IO.Path]::GetFullPath($WinPath)
    $p = $full -replace '\\', '/'
    if ($p -match '^([A-Za-z]):(.*)$') {
        return '/mnt/' + $Matches[1].ToLower() + $Matches[2]
    }
    return $p
}

function Write-UnixScript {
    param([string]$Path, [string[]]$Lines)
    $utf8 = New-Object System.Text.UTF8Encoding $false
    $text = ($Lines -join [char]10) + [char]10
    [System.IO.File]::WriteAllText($Path, $text, $utf8)
}

$grubScript = Join-Path $env:TEMP "myos_grub_install.sh"
Write-UnixScript $grubScript @(
    '#!/bin/bash'
    'set -e'
    'BOOT=/mnt/x/boot'
    'if [ ! -d "$BOOT" ]; then'
    '  echo "ERROR: /mnt/x/boot not found. Keep pen drive plugged in (X: in Explorer)."'
    '  echo "Try: wsl --shutdown   then run this script again."'
    '  exit 1'
    'fi'
    'for dev in /dev/sdb /dev/sdc /dev/sdd /dev/sde; do'
    '  if [ -b "$dev" ]; then'
    '    echo "Trying grub-install on $dev ..."'
    '    if grub-install --target=i386-pc --boot-directory="$BOOT" --no-floppy --force "$dev"; then'
    '      echo "Installed to $dev"'
    '      exit 0'
    '    fi'
    '  fi'
    'done'
    'echo "grub-install failed on sdb-sde"'
    'exit 1'
)

$wslScript = Get-WslPath $grubScript
Write-Host "Installing GRUB to USB (X: = /mnt/x/boot) ..." -ForegroundColor Yellow
Write-Host "    Script: $wslScript" -ForegroundColor Gray

$out = wsl bash $wslScript 2>&1 | Out-String
Write-Host $out.Trim()

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "OK. Reboot PC and select USB boot in BIOS." -ForegroundColor Green
} else {
    Write-Host ""
    Write-Host "Failed. Try:" -ForegroundColor Red
    Write-Host "  1. wsl --shutdown" -ForegroundColor Yellow
    Write-Host "  2. Plug pen drive, check X: in Explorer" -ForegroundColor Yellow
    Write-Host "  3. Run this script again" -ForegroundColor Yellow
}
