# restore_usb_letters.ps1 - Put X: and Y: back on the myos pen drive (Disk 1)
# Run as Administrator.

$ErrorActionPreference = "Stop"

if (-NOT ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]"Administrator")) {
    Write-Host "[ERROR] Run PowerShell as Administrator." -ForegroundColor Red
    exit 1
}

$disk = Get-CimInstance -Query "SELECT * FROM Win32_DiskDrive WHERE Index=1"
if (-not $disk) {
    Write-Host "[ERROR] PHYSICALDRIVE1 not found. Plug in the pen drive." -ForegroundColor Red
    exit 1
}

Write-Host "Disk 1: $($disk.Model) ($([math]::Round($disk.Size/1GB,1)) GB)" -ForegroundColor Cyan

$dp = @"
select disk 1
select partition 1
assign letter=X
select partition 2
assign letter=Y
exit
"@
$dpFile = Join-Path $env:TEMP "myos_restore_letters.txt"
[System.IO.File]::WriteAllText($dpFile, $dp, [System.Text.Encoding]::ASCII)
diskpart /s $dpFile
Remove-Item $dpFile -Force -ErrorAction SilentlyContinue

Start-Sleep -Seconds 2

if (Test-Path "X:\boot\myos.bin") {
    Write-Host "OK: X:\boot\myos.bin found" -ForegroundColor Green
} else {
    Write-Host "[WARN] X:\boot\myos.bin still missing. Run flash_windows.ps1 again." -ForegroundColor Yellow
}

if (Test-Path "Y:\") {
    Write-Host "OK: Y: drive visible" -ForegroundColor Green
} else {
    Write-Host "[WARN] Y: not visible (optional)" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Next: powershell -File install_grub_only.ps1" -ForegroundColor Cyan
