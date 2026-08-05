#include "fat32.h"
#include "screen.h"
#include "libc/string.h"

static file_entry_t file_system[MAX_FILES];
static int is_fat32_mounted = 0;

static int strcasecmp_local(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = *s1;
        char c2 = *s2;
        if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
        if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    char c1 = *s1;
    char c2 = *s2;
    if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
    if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
    return c1 - c2;
}

void fat32_init(void) {
    if (is_fat32_mounted) return;
    
    memset(file_system, 0, sizeof(file_system));

    // Pre-populate default files
    // 1. README.TXT
    strcpy(file_system[0].filename, "README.TXT");
    strcpy(file_system[0].content, "Welcome to SOSBasic OS!\nCreated by Shayak.\nType 'help' for commands.\n");
    file_system[0].size = strlen(file_system[0].content);
    file_system[0].is_directory = 0;
    file_system[0].in_use = 1;

    // 2. MAIN.C
    strcpy(file_system[1].filename, "MAIN.C");
    strcpy(file_system[1].content, "int main() {\n    printf(\"Hello World from real C file on FAT32!\\n\");\n    return 0;\n}\n");
    file_system[1].size = strlen(file_system[1].content);
    file_system[1].is_directory = 0;
    file_system[1].in_use = 1;

    // 3. DOCS/
    strcpy(file_system[2].filename, "DOCS");
    file_system[2].content[0] = '\0';
    file_system[2].size = 0;
    file_system[2].is_directory = 1;
    file_system[2].in_use = 1;

    is_fat32_mounted = 1;
}

void fat32_list_files(void) {
    if (!is_fat32_mounted) fat32_init();

    screen_puts("Directory listing of /storage/usb0:\n");
    int count = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (file_system[i].in_use) {
            count++;
            if (file_system[i].is_directory) {
                screen_puts("  [DIR ]  ");
                screen_puts(file_system[i].filename);
                screen_puts("/\n");
            } else {
                screen_puts("  [FILE]  ");
                screen_puts(file_system[i].filename);
                screen_puts("    (");
                screen_putnum((uint32_t)file_system[i].size, 10);
                screen_puts(" bytes)\n");
            }
        }
    }
    if (count == 0) {
        screen_puts("  (directory is empty)\n");
    }
}

const char *fat32_get_file_content(const char *filename) {
    if (!is_fat32_mounted) fat32_init();

    for (int i = 0; i < MAX_FILES; i++) {
        if (file_system[i].in_use && !file_system[i].is_directory) {
            if (strcasecmp_local(file_system[i].filename, filename) == 0) {
                return file_system[i].content;
            }
        }
    }
    return NULL;
}

int fat32_read_file(const char *filename) {
    const char *content = fat32_get_file_content(filename);
    if (content) {
        screen_puts("Reading ");
        screen_puts(filename);
        screen_puts(":\n----------------------------------------------------\n");
        screen_puts(content);
        if (content[strlen(content) - 1] != '\n') {
            screen_putc('\n');
        }
        screen_puts("----------------------------------------------------\n");
        return 1;
    } else {
        screen_puts("Error: File '");
        screen_puts(filename);
        screen_puts("' not found.\n");
        return 0;
    }
}

int fat32_create_file(const char *filename, const char *content) {
    if (!is_fat32_mounted) fat32_init();

    // Check if file already exists
    for (int i = 0; i < MAX_FILES; i++) {
        if (file_system[i].in_use && !file_system[i].is_directory) {
            if (strcasecmp_local(file_system[i].filename, filename) == 0) {
                if (content) {
                    strncpy(file_system[i].content, content, MAX_FILE_SIZE - 1);
                    file_system[i].content[MAX_FILE_SIZE - 1] = '\0';
                    file_system[i].size = strlen(file_system[i].content);
                } else {
                    file_system[i].content[0] = '\0';
                    file_system[i].size = 0;
                }
                screen_puts("Updated file '");
                screen_puts(file_system[i].filename);
                screen_puts("' on USB storage.\n");
                return 1;
            }
        }
    }

    // Find free slot
    for (int i = 0; i < MAX_FILES; i++) {
        if (!file_system[i].in_use) {
            strncpy(file_system[i].filename, filename, MAX_FILENAME_LEN - 1);
            file_system[i].filename[MAX_FILENAME_LEN - 1] = '\0';

            if (content) {
                strncpy(file_system[i].content, content, MAX_FILE_SIZE - 1);
                file_system[i].content[MAX_FILE_SIZE - 1] = '\0';
                file_system[i].size = strlen(file_system[i].content);
            } else {
                file_system[i].content[0] = '\0';
                file_system[i].size = 0;
            }

            file_system[i].is_directory = 0;
            file_system[i].in_use = 1;

            screen_puts("Created file '");
            screen_puts(file_system[i].filename);
            screen_puts("' on USB storage.\n");
            return 1;
        }
    }

    screen_puts("Error: Storage space full.\n");
    return 0;
}

int fat32_delete_file(const char *filename) {
    if (!is_fat32_mounted) fat32_init();

    for (int i = 0; i < MAX_FILES; i++) {
        if (file_system[i].in_use && !file_system[i].is_directory) {
            if (strcasecmp_local(file_system[i].filename, filename) == 0) {
                file_system[i].in_use = 0;
                screen_puts("Removed file '");
                screen_puts(filename);
                screen_puts("' from USB storage.\n");
                return 1;
            }
        }
    }

    screen_puts("Error: File '");
    screen_puts(filename);
    screen_puts("' not found.\n");
    return 0;
}

int fat32_make_directory(const char *dirname) {
    if (!is_fat32_mounted) fat32_init();

    for (int i = 0; i < MAX_FILES; i++) {
        if (!file_system[i].in_use) {
            strncpy(file_system[i].filename, dirname, MAX_FILENAME_LEN - 1);
            file_system[i].filename[MAX_FILENAME_LEN - 1] = '\0';
            file_system[i].content[0] = '\0';
            file_system[i].size = 0;
            file_system[i].is_directory = 1;
            file_system[i].in_use = 1;

            screen_puts("Created directory '");
            screen_puts(dirname);
            screen_puts("/' on USB storage.\n");
            return 1;
        }
    }

    screen_puts("Error: Storage space full.\n");
    return 0;
}

int fat32_remove_directory(const char *dirname) {
    if (!is_fat32_mounted) fat32_init();

    for (int i = 0; i < MAX_FILES; i++) {
        if (file_system[i].in_use && file_system[i].is_directory) {
            if (strcasecmp_local(file_system[i].filename, dirname) == 0) {
                file_system[i].in_use = 0;
                screen_puts("Removed directory '");
                screen_puts(dirname);
                screen_puts("/' from USB storage.\n");
                return 1;
            }
        }
    }

    screen_puts("Error: Directory '");
    screen_puts(dirname);
    screen_puts("' not found.\n");
    return 0;
}
