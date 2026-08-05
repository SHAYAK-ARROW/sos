# SOSBasic Shell OS (Bare-Metal Custom Operating System)

Welcome to **SOSBasic OS** — a custom 32-bit bare-metal operating system built from scratch in C and x86 Assembly.

---

## Features
- **Bare-Metal C Kernel**: Custom kernel with Multiboot compliance.
- **Display Controller (VGA & HDMI)**: Linear VBE/VGA framebuffer driver working on both VGA CRT/LCD monitors and HDMI displays.
- **Input Controller (PS/2 & USB Keyboard/Mouse)**: Full keyboard driver supporting native PS/2 and USB Keyboards (via BIOS Legacy USB Emulation & USB HID parser).
- **USB & PCI Stack**: PCI bus scanner detecting UHCI, OHCI, EHCI (USB 2.0), and xHCI (USB 3.0) controllers.
- **FAT32 USB Pendrive Support**: FAT32 file system driver to read/write files on Partition 2 of the Live USB Pendrive.
- **SOSBasic CLI Shell**: Interactive command line interface featuring commands: `help`, `clear`, `info`, `pci`, `usb`, `ls`, `cat`, `write`, `reboot`, `shutdown`.

---

## File Structure
- `boot.S` - Assembly Multiboot Header and Kernel Entry Point
- `kernel.c` - Main Kernel Initialization & Interactive Event Loop
- `screen.h` / `screen.c` - VGA/VBE Video Framebuffer Driver
- `io.h` - x86 Assembly Port Input/Output Subroutines
- `pci.h` / `pci.c` - PCI Bus Scanner & Device Detection
- `usb.h` / `usb.c` - USB Host Controller Interface & Mass Storage BOT Driver
- `keyboard.h` / `keyboard.c` - PS/2 & USB HID Keyboard Handler
- `fat32.h` / `fat32.c` - FAT32 Partition File System Driver
- `shell.h` / `shell.c` - SOSBasic Interactive CLI Shell Command Parser
- `linker.ld` - Memory Layout Linker Script
- `Makefile` / `build.sh` - Compilation Scripts

---

## How to Build
To compile the kernel:
```bash
make
```
This produces `sosbasic.bin`.

---

## How to Create the Live USB Pendrive (OS + Data Storage)

To make a USB drive where part of it runs **SOSBasic OS** and the rest is usable as a standard **Storage Drive**:

### Step 1: Partition the USB Pendrive
Use `GParted` (Linux) or `diskpart` (Windows):
1. **Partition 1 (OS Boot):**
   - Size: 1 GB
   - Type: FAT32
   - Name: `SOSBASIC_BOOT`
   - Flags: `bootable`
2. **Partition 2 (Data Storage):**
   - Size: Remaining Pendrive Space (e.g. 14 GB, 28 GB, 60 GB)
   - Type: FAT32 or exFAT
   - Name: `DATA_STORAGE`

### Step 2: Install GRUB Bootloader to Partition 1
```bash
sudo mount /dev/sdX1 /mnt
sudo grub-install --target=i386-pc --boot-directory=/mnt/boot /dev/sdX
sudo cp sosbasic.bin /mnt/boot/
```

Create `/mnt/boot/grub/grub.cfg`:
```
set timeout=5
set default=0

menuentry "SOSBasic Shell OS" {
    multiboot /boot/sosbasic.bin
    boot
}
```

### Step 3: Boot SOSBasic OS
1. Insert the Pendrive into any PC/Laptop.
2. In BIOS/UEFI setup, enable **Legacy Boot / CSM Mode** and **Legacy USB Support**.
3. Select the Pendrive as the primary boot device.
4. **SOSBasic Shell OS** will boot! Partition 2 will remain completely accessible as regular USB storage on Windows/Mac/Linux!
