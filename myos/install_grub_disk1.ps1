# install_grub_disk1.ps1 - GRUB on PHYSICALDRIVE1 (the 58GB pen drive from flash script)
# Run PowerShell as Administrator. Only the myos pen drive should be Disk 1 in Disk Management.

$ErrorActionPreference = "Continue"

if (-NOT ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]"Administrator")) {
    Write-Host "[ERROR] Run as Administrator." -ForegroundColor Red
    exit 1
}

if (-not (Test-Path "X:\boot\myos.bin")) {
    Write-Host "[WARN] X: drive letter missing (you ran mountvol X: /D)." -ForegroundColor Yellow
    Write-Host "       Run first: powershell -File restore_usb_letters.ps1" -ForegroundColor Yellow
    exit 1
}

$disk = Get-CimInstance -Query "SELECT * FROM Win32_DiskDrive WHERE Index=1"
if (-not $disk) {
    Write-Host "[ERROR] PHYSICALDRIVE1 not found." -ForegroundColor Red
    exit 1
}
$sizeGB = [math]::Round($disk.Size / 1GB, 1)
Write-Host "Target: PHYSICALDRIVE1 = $($disk.Model) ($sizeGB GB)" -ForegroundColor Cyan
Write-Host "If this is NOT your pen drive, STOP (wrong disk)." -ForegroundColor Yellow
$ok = Read-Host "Type YES if this is the myos pen drive"
if ($ok -notmatch '^(?i)yes$') { exit 0 }

function Write-UnixScript {
    param([string]$Path, [string[]]$Lines)
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($Path, (($Lines -join [char]10) + [char]10), $utf8)
}

function Get-WslPath {
    param([string]$WinPath)
    $full = [System.IO.Path]::GetFullPath($WinPath)
    $p = $full -replace '\\', '/'
    if ($p -match '^([A-Za-z]):(.*)$') { return '/mnt/' + $Matches[1].ToLower() + $Matches[2] }
    return $p
}

Write-Host ""
Write-Host "Unmounting WSL disks (if any)..." -ForegroundColor Gray
wsl --unmount \\.\PHYSICALDRIVE1 2>&1 | Out-Null

Write-Host "Mounting PHYSICALDRIVE1 in WSL (bare)..." -ForegroundColor Yellow
$mountOut = wsl --mount \\.\PHYSICALDRIVE1 --bare 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] wsl --mount failed: $($mountOut.Trim())" -ForegroundColor Red
    Write-Host "Try: wsl --shutdown   then run this script again." -ForegroundColor Yellow
    exit 1
}

$grubScript = Join-Path $env:TEMP "myos_grub_pd1.sh"
Write-UnixScript $grubScript @(
    '#!/bin/bash'
    'set -e'
    'BOOT=/mnt/x/boot'
    'if [ ! -f "$BOOT/myos.bin" ]; then'
    '  echo "ERROR: $BOOT/myos.bin missing. X: must be mounted."'
    '  exit 1'
    'fi'
    'echo "Block devices:"'
    'lsblk -d -o NAME,SIZE,MODEL'
    'DEV=""'
    'for d in /dev/sd?; do'
    '  [ -b "$d" ] || continue'
    '  sz=$(blockdev --getsize64 "$d" 2>/dev/null || echo 0)'
    '  if [ "$sz" -gt 50000000000 ] && [ "$sz" -lt 70000000000 ]; then'
    '    DEV="$d"'
    '    break'
    '  fi'
    'done'
    'if [ -z "$DEV" ]; then'
    '  DEV=$(lsblk -dnp -o NAME,SIZE | awk ''$2 > 50000000000 && $2 < 70000000000 {print "/dev/"$1; exit}'')'
    'fi'
    'if [ -z "$DEV" ]; then'
    '  echo "ERROR: cannot find ~58GB USB disk in WSL. Using /dev/sdb as fallback."'
    '  DEV=/dev/sdb'
    'fi'
    'echo "Installing GRUB to $DEV (boot files in $BOOT) ..."'
    'grub-install --target=i386-pc --boot-directory="$BOOT" --no-floppy --force "$DEV"'
    'echo "OK: grub-install finished on $DEV"'
)

$wslScript = Get-WslPath $grubScript
$out = wsl bash $wslScript 2>&1 | Out-String
Write-Host $out.Trim()

wsl --unmount \\.\PHYSICALDRIVE1 2>&1 | Out-Null

if ($out -match 'OK: grub-install finished') {
    Write-Host ""
    Write-Host "Done. Reboot -> boot menu -> USB DISK 2.0 PMAP (NOT UEFI)." -ForegroundColor Green
} else {
    Write-Host ""
    Write-Host "May have failed. Send the output above." -ForegroundColor Red
}
