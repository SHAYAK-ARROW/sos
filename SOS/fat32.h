#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stddef.h>

#define MAX_FILES 64
#define MAX_FILENAME_LEN 32
#define MAX_FILE_SIZE 2048

typedef struct {
    char filename[MAX_FILENAME_LEN];
    char content[MAX_FILE_SIZE];
    size_t size;
    int is_directory;
    int in_use;
} file_entry_t;

void fat32_init(void);
void fat32_list_files(void);
int fat32_read_file(const char *filename);
const char *fat32_get_file_content(const char *filename);
int fat32_create_file(const char *filename, const char *content);
int fat32_delete_file(const char *filename);
int fat32_make_directory(const char *dirname);
int fat32_remove_directory(const char *dirname);

#endif
