#include "shell.h"
#include "screen.h"
#include "fat32.h"
#include "fat32_parser.h"
#include "fat32_dir.h"
#include "pci.h"
#include "usb.h"
#include "keyboard.h"
#include "compiler.h"
#include "io.h"
#include "libc/stdio.h"
#include "libc/string.h"

static int has_c_extension(const char *filename) {
    int len = (int)strlen(filename);
    if (len < 3) return 0;
    if (filename[len - 2] == '.' && (filename[len - 1] == 'c' || filename[len - 1] == 'C')) {
        return 1;
    }
    return 0;
}

static void save_file_to_disk_or_memory(const char *filename, const char *content) {
    const fat32_info_t *info = fat32_get_info();
    uint32_t size = (uint32_t)strlen(content);

    if (info && info->is_mounted) {
        uint32_t start_clus = 0;
        uint32_t file_size = 0;
        uint8_t attr = 0;
        uint32_t cur_dir = fat32_get_current_dir_cluster();

        if (fat32_find_entry(cur_dir, filename, &start_clus, &file_size, &attr)) {
            if (!(attr & FAT32_ATTR_DIRECTORY)) {
                // Update existing file content on disk
                if (fat32_write_file_content(start_clus, (const uint8_t *)content, size)) {
                    printf("Updated file '%s' (%u bytes) on FAT32 USB drive.\n", filename, size);
                }
                return;
            }
        }

        // Create new file on physical FAT32 disk
        uint32_t new_clus = fat32_allocate_cluster();
        if (new_clus == 0) return;

        if (size > 0) {
            fat32_write_file_content(new_clus, (const uint8_t *)content, size);
        }

        if (fat32_write_dir_entry(cur_dir, filename, FAT32_ATTR_ARCHIVE, new_clus, size)) {
            printf("Created file '%s' (%u bytes) on FAT32 USB drive.\n", filename, size);
        }
    } else {
        fat32_create_file(filename, content); // Fallback to in-memory
    }
}

static void run_nano_editor(const char *filename) {
    screen_clear();
    screen_set_color(COLOR_WHITE, COLOR_BLACK);
    screen_puts("====================================================\n");
    screen_puts("           SOSBasic Nano Text Editor               \n");
    screen_puts(" Editing File: ");
    screen_puts(filename);
    screen_puts("\n====================================================\n");

    const fat32_info_t *info = fat32_get_info();
    uint32_t start_clus = 0, file_size = 0;
    uint8_t attr = 0;
    uint8_t existing_buf[1024];
    memset(existing_buf, 0, sizeof(existing_buf));

    if (info && info->is_mounted && fat32_find_entry(fat32_get_current_dir_cluster(), filename, &start_clus, &file_size, &attr)) {
        if (!(attr & FAT32_ATTR_DIRECTORY) && file_size > 0) {
            fat32_read_file_content(start_clus, file_size, existing_buf, sizeof(existing_buf) - 1);
            screen_puts("Current Content:\n");
            screen_puts((const char *)existing_buf);

            // Bug Fix: Guard against strlen == 0 underflow crash
            uint32_t clen = (uint32_t)strlen((const char *)existing_buf);
            if (clen > 0 && existing_buf[clen - 1] != '\n') {
                screen_putc('\n');
            }
            screen_puts("----------------------------------------------------\n");
        }
    } else {
        const char *mem_content = fat32_get_file_content(filename);
        if (mem_content && mem_content[0] != '\0') {
            screen_puts("Current Content:\n");
            screen_puts(mem_content);
            uint32_t mlen = (uint32_t)strlen(mem_content);
            if (mlen > 0 && mem_content[mlen - 1] != '\n') {
                screen_putc('\n');
            }
            screen_puts("----------------------------------------------------\n");
        }
    }

    screen_puts("Type new text below.\nType ':w' on a new line and press Enter to Save & Exit.\n");
    screen_puts("----------------------------------------------------\n\n");

    char editor_buf[1024];
    int buf_idx = 0;

    char line_buf[128];
    int line_idx = 0;

    while (1) {
        char c = keyboard_get_char();
        if (c != 0) {
            if (c == '\n') {
                line_buf[line_idx] = '\0';
                screen_putc('\n');

                if (strcmp(line_buf, ":w") == 0 || strcmp(line_buf, ":q") == 0) {
                    editor_buf[buf_idx] = '\0';
                    break;
                }

                for (int i = 0; i < line_idx; i++) {
                    if (buf_idx < 1000) editor_buf[buf_idx++] = line_buf[i];
                }
                if (buf_idx < 1000) editor_buf[buf_idx++] = '\n';

                line_idx = 0;
            } else if (c == '\b') {
                if (line_idx > 0) {
                    line_idx--;
                    screen_putc('\b');
                }
            } else {
                if (line_idx < 127) {
                    line_buf[line_idx++] = c;
                    screen_putc(c);
                }
            }
        }
    }

    save_file_to_disk_or_memory(filename, editor_buf);
    screen_puts("\nPress Enter to return to SOSBasic Shell...");
    while (1) {
        char c = keyboard_get_char();
        if (c == '\n') break;
    }

    shell_init();
}

void shell_init(void) {
    screen_clear();
    screen_set_color(COLOR_WHITE, COLOR_BLACK);

    screen_puts("\n");
    screen_puts("   ___   ___   ___ \n");
    screen_puts("  / __| / _ \\ / __|\n");
    screen_puts("  \\__ \\| (_) |\\__ \\\n");
    screen_puts("  |___/ \\___/ |___/\n");
    screen_puts("     By Shayak\n");
    screen_puts("-----------------------\n");
    screen_puts("Type 'help' for commands.\n\n");

    screen_puts("sosbasic> ");
}

void shell_process_command(const char *cmd_line) {
    if (cmd_line[0] == '\0') {
        screen_puts("\nsosbasic> ");
        return;
    }

    screen_puts("\n");

    const fat32_info_t *info = fat32_get_info();

    if (strcmp(cmd_line, "help") == 0) {
        screen_puts("SOSBasic Commands:\n");
        screen_puts("  cd <dir>     - Change directory\n");
        screen_puts("  pwd          - Print working directory\n");
        screen_puts("  ls           - List files & directories in current path\n");
        screen_puts("  cat <file>   - Read & view file content from FAT32\n");
        screen_puts("  write <file> - Write file content to FAT32 disk\n");
        screen_puts("  gcc <file.c> - Compile & run C file from FAT32\n");
        screen_puts("  nano <file>  - Edit file in Nano text editor\n");
        screen_puts("  mkdir <dir>  - Create a directory\n");
        screen_puts("  rmdir <dir>  - Remove a directory\n");
        screen_puts("  touch <file> - Create empty file\n");
        screen_puts("  rm <file>    - Remove/delete a file\n");
        screen_puts("  echo <text>  - Print text message\n");
        screen_puts("  clear        - Clear screen\n");
        screen_puts("  info         - Show system architecture\n");
        screen_puts("  pci / usb    - Scan hardware & USB devices\n");
        screen_puts("  reboot       - Restart system\n");
        screen_puts("  shutdown     - Power off / Halt CPU\n");
    } else if (strcmp(cmd_line, "cd") == 0 || strncmp(cmd_line, "cd ", 3) == 0) {
        if (strncmp(cmd_line, "cd ", 3) == 0 && cmd_line[3] != '\0') {
            fat32_change_dir(cmd_line + 3);
        } else {
            fat32_change_dir("/");
        }
    } else if (strcmp(cmd_line, "pwd") == 0) {
        printf("%s\n", fat32_get_current_path());
    } else if (strcmp(cmd_line, "ls") == 0) {
        if (info && info->is_mounted) {
            fat32_list_directory(fat32_get_current_dir_cluster());
        } else {
            fat32_list_files(); // Fallback
        }
    } else if (strcmp(cmd_line, "write") == 0 || strncmp(cmd_line, "write ", 6) == 0) {
        const char *filename = "TEST.TXT";
        if (strncmp(cmd_line, "write ", 6) == 0 && cmd_line[6] != '\0') {
            filename = cmd_line + 6;
        }
        save_file_to_disk_or_memory(filename, "Hello from SOSBasic OS Physical FAT32 USB Write!\n");
    } else if (strcmp(cmd_line, "gcc") == 0 || strncmp(cmd_line, "gcc ", 4) == 0) {
        if (strncmp(cmd_line, "gcc ", 4) == 0 && cmd_line[4] != '\0') {
            const char *filename = cmd_line + 4;
            if (has_c_extension(filename)) {
                uint32_t start_clus = 0, file_size = 0;
                uint8_t attr = 0;
                uint8_t c_src_buf[2048];

                if (info && info->is_mounted && fat32_find_entry(fat32_get_current_dir_cluster(), filename, &start_clus, &file_size, &attr) && !(attr & FAT32_ATTR_DIRECTORY)) {
                    memset(c_src_buf, 0, sizeof(c_src_buf));
                    if (fat32_read_file_content(start_clus, file_size, c_src_buf, sizeof(c_src_buf) - 1)) {
                        gcc_compile_and_run(filename, (const char *)c_src_buf);
                    } else {
                        printf("Error: Failed to read file '%s' from FAT32 disk.\n", filename);
                    }
                } else {
                    const char *real_content = fat32_get_file_content(filename);
                    if (real_content) {
                        gcc_compile_and_run(filename, real_content);
                    } else {
                        printf("Error: File '%s' not found on FAT32 storage.\n", filename);
                    }
                }
            } else {
                screen_puts("Error: GCC requires a file with '.c' extension (e.g., 'gcc main.c').\n");
            }
        } else {
            screen_puts("Error: Please specify a .c file (e.g., 'gcc main.c').\n");
        }
    } else if (strcmp(cmd_line, "cat") == 0 || strncmp(cmd_line, "cat ", 4) == 0) {
        if (strncmp(cmd_line, "cat ", 4) == 0 && cmd_line[4] != '\0') {
            const char *filename = cmd_line + 4;
            uint32_t start_clus = 0, file_size = 0;
            uint8_t attr = 0;
            uint8_t file_buf[2048];

            if (info && info->is_mounted && fat32_find_entry(fat32_get_current_dir_cluster(), filename, &start_clus, &file_size, &attr) && !(attr & FAT32_ATTR_DIRECTORY)) {
                memset(file_buf, 0, sizeof(file_buf));
                printf("Reading %s from FAT32 Disk:\n----------------------------------------------------\n", filename);
                if (fat32_read_file_content(start_clus, file_size, file_buf, sizeof(file_buf) - 1)) {
                    printf("%s\n----------------------------------------------------\n", file_buf);
                } else {
                    printf("Error: Failed to read file content.\n");
                }
            } else {
                fat32_read_file(filename); // Fallback
            }
        } else {
            fat32_read_file("README.TXT");
        }
    } else if (strcmp(cmd_line, "mkdir") == 0 || strncmp(cmd_line, "mkdir ", 6) == 0) {
        const char *dirname = "NEWDIR";
        if (strncmp(cmd_line, "mkdir ", 6) == 0 && cmd_line[6] != '\0') {
            dirname = cmd_line + 6;
        }

        if (info && info->is_mounted) {
            uint32_t new_clus = fat32_allocate_cluster();
            if (new_clus > 0) {
                if (fat32_write_dir_entry(fat32_get_current_dir_cluster(), dirname, FAT32_ATTR_DIRECTORY, new_clus, 0)) {
                    printf("Created directory '%s/' on FAT32 USB drive.\n", dirname);
                }
            }
        } else {
            fat32_make_directory(dirname); // Fallback
        }
    } else if (strcmp(cmd_line, "rmdir") == 0 || strncmp(cmd_line, "rmdir ", 6) == 0) {
        const char *dirname = "NEWDIR";
        if (strncmp(cmd_line, "rmdir ", 6) == 0 && cmd_line[6] != '\0') {
            dirname = cmd_line + 6;
        }

        if (info && info->is_mounted) {
            if (fat32_delete_entry(fat32_get_current_dir_cluster(), dirname)) {
                printf("Removed directory '%s/' from FAT32 USB drive.\n", dirname);
            }
        } else {
            fat32_remove_directory(dirname); // Fallback
        }
    } else if (strcmp(cmd_line, "touch") == 0 || strncmp(cmd_line, "touch ", 6) == 0) {
        const char *filename = "NEWFILE.TXT";
        if (strncmp(cmd_line, "touch ", 6) == 0 && cmd_line[6] != '\0') {
            filename = cmd_line + 6;
        }

        save_file_to_disk_or_memory(filename, "");
    } else if (strcmp(cmd_line, "nano") == 0 || strncmp(cmd_line, "nano ", 5) == 0) {
        if (strncmp(cmd_line, "nano ", 5) == 0 && cmd_line[5] != '\0') {
            run_nano_editor(cmd_line + 5);
            return;
        } else {
            run_nano_editor("MAIN.C");
            return;
        }
    } else if (strcmp(cmd_line, "rm") == 0 || strncmp(cmd_line, "rm ", 3) == 0 ||
               strcmp(cmd_line, "delete") == 0 || strncmp(cmd_line, "delete ", 7) == 0) {
        const char *filename = NULL;
        if (strncmp(cmd_line, "rm ", 3) == 0 && cmd_line[3] != '\0') {
            filename = cmd_line + 3;
        } else if (strncmp(cmd_line, "delete ", 7) == 0 && cmd_line[7] != '\0') {
            filename = cmd_line + 7;
        }

        if (filename) {
            if (info && info->is_mounted) {
                fat32_delete_entry(fat32_get_current_dir_cluster(), filename);
            } else {
                fat32_delete_file(filename); // Fallback
            }
        } else {
            screen_puts("Error: Please specify a filename to delete (e.g., 'rm test.txt').\n");
        }
    } else if (strcmp(cmd_line, "echo") == 0 || strncmp(cmd_line, "echo ", 5) == 0) {
        if (strncmp(cmd_line, "echo ", 5) == 0 && cmd_line[5] != '\0') {
            screen_puts(cmd_line + 5);
            screen_puts("\n");
        } else {
            screen_puts("\n");
        }
    } else if (strcmp(cmd_line, "clear") == 0) {
        screen_clear();
        screen_puts("   ___   ___   ___ \n");
        screen_puts("  / __| / _ \\ / __|\n");
        screen_puts("  \\__ \\| (_) |\\__ \\\n");
        screen_puts("  |___/ \\___/ |___/\n");
        screen_puts("     By Shayak\n\n");
    } else if (strcmp(cmd_line, "info") == 0) {
        screen_puts("SOSBasic Shell OS (32-bit Bare-Metal GCC-enabled Kernel)\n");
        screen_puts("Author : Shayak\n");
        screen_puts("Display: VBE/VGA Framebuffer\n");
        screen_puts("Compiler: Built-in GCC C Engine\n");
    } else if (strcmp(cmd_line, "pci") == 0) {
        pci_scan_bus();
    } else if (strcmp(cmd_line, "usb") == 0) {
        screen_puts("USB Devices:\n  1. USB Mass Storage Flash Drive (BOT/SCSI)\n  2. USB HID Keyboard & Mouse\n");
    } else if (strcmp(cmd_line, "reboot") == 0) {
        screen_puts("Rebooting...\n");
        outb(0x64, 0xFE);
    } else if (strcmp(cmd_line, "shutdown") == 0) {
        screen_puts("System Halted. Safe to power off.\n");
        while(1) {
            asm volatile ("cli; hlt");
        }
    } else {
        screen_puts("Unknown command. Type 'help'.\n");
    }

    screen_puts("\nsosbasic> ");
}
