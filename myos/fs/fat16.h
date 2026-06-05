#ifndef FAT16_H
#define FAT16_H
#include <stdint.h>

#define FAT_MAX_FILENAME 64
#define FAT_MAX_FILES    32

typedef struct {
    char name[FAT_MAX_FILENAME];
    uint32_t size;
    uint32_t start_cluster;
    int is_dir;
} FatEntry;

int  fat_init(void);          /* init - format if needed */
int  fat_read(const char* path, char* buf, uint32_t maxlen);
int  fat_write(const char* path, const char* buf, uint32_t len);
int  fat_delete(const char* path);
int  fat_list(const char* dir, FatEntry* entries, int max);
int  fat_mkdir(const char* path);
int  fat_exists(const char* path);
int  fat_ready(void);         /* returns 1 if disk available */

#endif
