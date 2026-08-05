CC = gcc
AS = as
LD = ld

CFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra -I. -Ilibc
ASFLAGS = --32
LDFLAGS = -m elf_i386 -T linker.ld

OBJS = boot.o kernel.o screen.o pit.o pci.o usb_uhci.o usb_enum.o usb_storage.o fat32_parser.o fat32_dir.o usb.o keyboard.o fat32.o compiler.o shell.o \
       libc/stdio.o libc/stdlib.o libc/string.o libc/ctype.o

all: sosbasic.bin

boot.o: boot.S
	$(CC) -m32 -c boot.S -o boot.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

sosbasic.bin: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

clean:
	rm -f *.o libc/*.o sosbasic.bin sosbasic.iso
