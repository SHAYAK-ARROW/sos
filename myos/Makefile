# Makefile — MyOS i386 bare-metal build
CC      = i686-linux-gnu-gcc
AS      = i686-linux-gnu-as
LD      = i686-linux-gnu-ld

CFLAGS  = -std=gnu99 -m32 -ffreestanding -fno-stack-protector \
          -fno-pie -O2 -Wall -Wextra -I. -Wno-unused-function

LDFLAGS = -T linker.ld -m32 -ffreestanding -nostdlib -fno-pie \
          -no-pie -lgcc

SRCS_C = kernel/kernel.c \
         drivers/terminal.c \
         drivers/keyboard.c \
         drivers/usb_hid_kbd.c \
         drivers/ata.c \
         drivers/pci.c \
         drivers/usb.c \
         drivers/disk.c \
         fs/fs.c \
         fs/fat16.c \
         shell/shell.c \
         shell/cinterp.c \
         shell/tcc.c \
         mm/pmm.c \
         mm/paging.c \
         cpu/gdt.c \
         cpu/idt.c

SRCS_S = boot/boot.s

OBJS_C = $(SRCS_C:.c=.o)
OBJS_S = $(SRCS_S:.s=.o)
OBJS   = $(OBJS_S) $(OBJS_C)

# ── Primary targets ─────────────────────────────────────────────────────

all: myos.iso

myos.bin: $(OBJS) linker.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS)
	@echo "=== ELF verification ==="
	@file myos.bin
	@objdump -f myos.bin | grep -E 'architecture|start address'

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(AS) --32 $< -o $@

# ── ISO ──────────────────────────────────────────────────────────────────

myos.iso: myos.bin grub.cfg
	mkdir -p isodir/boot/grub
	cp myos.bin isodir/boot/myos.bin
	cp grub.cfg isodir/boot/grub/grub.cfg
	grub-mkrescue -o myos.iso isodir
	@grub-file --is-x86-multiboot isodir/boot/myos.bin && \
	    echo "GRUB CHECK PASSED: valid Multiboot 1 image"

# ── QEMU run targets ─────────────────────────────────────────────────────
# ISO দিয়ে boot, USB image attach — serial output terminal এ
qemu: myos.iso
	qemu-system-i386 \
	  -cdrom myos.iso \
	  -device qemu-xhci,id=xhci \
	  -device usb-kbd,bus=xhci.0 \
	  -drive if=none,id=usbstick,file=usb.img,format=raw \
	  -device usb-storage,bus=xhci.0,drive=usbstick \
	  -serial stdio \
	  -m 256 \
	  -no-reboot
# Debug mode — GDB server চালু, QEMU pause করে wait করে
qemu-debug: myos.iso
	qemu-system-i386 \
	  -cdrom myos.iso \
	  -device qemu-xhci,id=xhci \
	  -drive if=none,id=usbstick,file=usb.img,format=raw \
	  -device usb-storage,bus=xhci.0,drive=usbstick \
	  -serial stdio \
	  -m 256 \
	  -no-reboot \
	  -s -S

# Serial log file এ save করা
qemu-log: myos.iso
	qemu-system-i386 \
	  -cdrom myos.iso \
	  -device qemu-xhci,id=xhci \
	  -drive if=none,id=usbstick,file=usb.img,format=raw \
	  -device usb-storage,bus=xhci.0,drive=usbstick \
	  -serial file:serial.log \
	  -m 256 \
	  -no-reboot

# ── Clean ────────────────────────────────────────────────────────────────

clean:
	rm -f $(OBJS) myos.bin myos.iso
	rm -rf isodir

.PHONY: all clean qemu qemu-debug qemu-log
