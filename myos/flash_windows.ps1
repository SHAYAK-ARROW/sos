# flash_windows.ps1 - myos Pen Drive Flasher
# Run: PowerShell (Administrator)
#   cd "C:\Users\USER\Downloads\myos1\myos"
#   powershell -ExecutionPolicy Bypass -File .\flash_windows.ps1

$ErrorActionPreference = "Stop"

function Wait-DriveLetter {
    param([string]$Letter, [int]$Seconds = 30)
    $path = "${Letter}:\"
    for ($i = 0; $i -lt $Seconds; $i++) {
        if (Test-Path $path) { return $true }
        Start-Sleep -Seconds 1
    }
    return $false
}

Write-Host ""
Write-Host "======================================" -ForegroundColor Cyan
Write-Host "       myos Pen Drive Flasher         " -ForegroundColor Cyan
Write-Host "======================================" -ForegroundColor Cyan
Write-Host ""

if (-NOT ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]"Administrator")) {
    Write-Host "[ERROR] Run PowerShell as Administrator!" -ForegroundColor Red
    exit 1
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir
Write-Host "[1/5] myos folder: $ScriptDir" -ForegroundColor Yellow

$Drive = $ScriptDir.Substring(0,1).ToLower()
$Rest  = $ScriptDir.Substring(3).Replace("\", "/")
$WslPath = "/mnt/$Drive/$Rest"
Write-Host "      WSL path:    $WslPath" -ForegroundColor Gray

Write-Host "[2/5] Building myos via WSL..." -ForegroundColor Yellow
wsl bash -c "cd '$WslPath' && make clean -s && make -s"
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] Build failed!" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path "$ScriptDir\myos.bin")) {
    Write-Host "[ERROR] myos.bin not found!" -ForegroundColor Red
    exit 1
}
Write-Host "    OK: Build successful" -ForegroundColor Green

Write-Host "[3/5] Pen drive detected:" -ForegroundColor Yellow
$usbDisk = Get-CimInstance -Query "SELECT * FROM Win32_DiskDrive WHERE Index=1"
if (-not $usbDisk) {
    Write-Host "[ERROR] PHYSICALDRIVE1 not found. Check Disk Management." -ForegroundColor Red
    exit 1
}
$usbSizeGB = [math]::Round($usbDisk.Size / 1GB, 1)
Write-Host "    PHYSICALDRIVE1: $($usbDisk.Model) - $usbSizeGB GB" -ForegroundColor Cyan
Write-Host ""
Write-Host "!!! WARNING: ALL DATA ON THIS DRIVE WILL BE ERASED !!!" -ForegroundColor Red
Write-Host ""
$confirm = Read-Host "Type YES to confirm"
if ($confirm -notmatch '^(?i)yes$') {
    Write-Host "Cancelled." -ForegroundColor Yellow
    exit 0
}

Write-Host "[4/5] Creating partitions..." -ForegroundColor Yellow
Write-Host "    Both partitions use FAT32 (FAT16 fails on large USB)" -ForegroundColor Gray

$dpScript = "select disk 1`r`nclean`r`nconvert mbr`r`ncreate partition primary size=256`r`nactive`r`nformat fs=fat32 quick label=MYOSBOOT`r`nassign letter=X`r`ncreate partition primary`r`nformat fs=fat32 quick label=MYOSDATA`r`nassign letter=Y`r`nexit"
$dpFile = Join-Path $env:TEMP "myos_dp.txt"
[System.IO.File]::WriteAllText($dpFile, $dpScript, [System.Text.Encoding]::ASCII)
diskpart /s $dpFile
Remove-Item $dpFile -Force -ErrorAction SilentlyContinue

Start-Sleep -Seconds 3
Get-Volume -ErrorAction SilentlyContinue | Out-Null

if (-not (Wait-DriveLetter "X" 25)) {
    Write-Host "[ERROR] Drive X: (boot) not found after partition." -ForegroundColor Red
    exit 1
}
if (-not (Wait-DriveLetter "Y" 25)) {
    Write-Host "[WARN] Drive Y: not ready - trying diskpart assign..." -ForegroundColor Yellow
    $dpRetry = "select disk 1`r`nselect partition 2`r`nformat fs=fat32 quick label=MYOSDATA`r`nassign letter=Y`r`nexit"
    $dpRetryFile = Join-Path $env:TEMP "myos_dp_retry.txt"
    [System.IO.File]::WriteAllText($dpRetryFile, $dpRetry, [System.Text.Encoding]::ASCII)
    diskpart /s $dpRetryFile | Out-Null
    Remove-Item $dpRetryFile -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
}

$hasX = Test-Path "X:\"
$hasY = Test-Path "Y:\"
Write-Host "    Boot partition X: = $(if ($hasX) { 'OK' } else { 'MISSING' })" -ForegroundColor $(if ($hasX) { 'Green' } else { 'Red' })
if ($hasY) {
    Write-Host "    Data partition Y: = OK" -ForegroundColor Green
} else {
    Write-Host "    Data partition Y: = MISSING (optional; myos uses partition 2 via MBR)" -ForegroundColor Yellow
}

if (-not $hasX) {
    Write-Host "[ERROR] Cannot continue without X: boot drive." -ForegroundColor Red
    exit 1
}

Write-Host "[5/5] Installing GRUB and kernel..." -ForegroundColor Yellow

New-Item -ItemType Directory -Force -Path "X:\boot\grub" | Out-Null

$grubCfg = "set timeout=3`r`nset default=0`r`n`r`nmenuentry `"myos`" {`r`n    multiboot /boot/myos.bin`r`n    boot`r`n}`r`n"
[System.IO.File]::WriteAllText("X:\boot\grub\grub.cfg", $grubCfg, [System.Text.Encoding]::ASCII)
Copy-Item "$ScriptDir\myos.bin" "X:\boot\myos.bin" -Force
Write-Host "    OK: myos.bin copied to X:\boot\" -ForegroundColor Gray

$grubHelper = Join-Path $ScriptDir "install_grub_only.ps1"
if (Test-Path $grubHelper) {
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $grubHelper
    $grubExit = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    if ($grubExit -ne 0) {
        Write-Host "    WARN: GRUB install failed (see above)." -ForegroundColor Yellow
    }
} else {
    Write-Host "    WARN: install_grub_only.ps1 missing; skip GRUB." -ForegroundColor Yellow
}

if ($hasY) {
    [System.IO.File]::WriteAllText("Y:\README.TXT", "myos Data Partition`r`nFiles from the shell are stored on partition 2.", [System.Text.Encoding]::ASCII)
    Write-Host "    OK: Y:\README.TXT" -ForegroundColor Gray
} else {
    Write-Host "    SKIP: Y:\ not available (README only, optional)" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "======================================" -ForegroundColor Green
Write-Host "  DONE! Pen drive should be ready.     " -ForegroundColor Green
Write-Host "  Boot from USB in BIOS (Legacy USB).  " -ForegroundColor Green
Write-Host "======================================" -ForegroundColor Green
Write-Host ""
Write-Host "  X:\ = Boot (GRUB + myos.bin)" -ForegroundColor White
if ($hasY) {
    Write-Host "  Y:\ = Data (partition 2 for myos storage)" -ForegroundColor White
}
Write-Host ""
