#include <stdint.h>
#include "screen.h"
#include "pit.h"
#include "pci.h"
#include "usb.h"
#include "keyboard.h"
#include "fat32.h"
#include "shell.h"

void kernel_main(uint32_t magic, uint32_t multiboot_addr) {
    (void)magic;
    (void)multiboot_addr;

    // 1. Initialize Display
    screen_init();

    // 2. Initialize PIT Hardware Timer (1000 Hz = 1 ms precision)
    pit_init(1000);

    // 3. Initialize PCI Bus Scanning & USB Drivers
    pci_init();

    // 4. Initialize USB Driver Stack
    usb_init();

    // 5. Initialize Keyboard
    keyboard_init();

    // 6. Initialize FAT32 USB Storage
    fat32_init();

    // Pause prompt to let user review all boot-time USB storage, SCSI & PCI logs
    screen_puts("\nBoot Complete. Press Enter to continue to Shell...\n");
    while (keyboard_get_char() != '\n');

    // 7. Launch SOSBasic Shell
    shell_init();

    char line_buffer[128];
    int line_index = 0;

    // 8. Interactive Kernel Execution Loop
    while (1) {
        char c = keyboard_get_char();
        if (c != 0) {
            if (c == '\n') {
                line_buffer[line_index] = '\0';
                shell_process_command(line_buffer);
                line_index = 0;
            } else if (c == '\b') {
                if (line_index > 0) {
                    line_index--;
                    screen_putc('\b');
                }
            } else {
                if (line_index < 127) {
                    line_buffer[line_index++] = c;
                    screen_putc(c);
                }
            }
        }
    }
}
