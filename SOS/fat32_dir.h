#ifndef FAT32_DIR_H
#define FAT32_DIR_H

#include <stdint.h>
#include <stddef.h>

// FAT32 Attributes
#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LFN       0x0F // Long File Name

// Standard FAT32 32-Byte Directory Entry
typedef struct {
    char     DIR_Name[11];      // Offset 0: Short Filename (8 chars name + 3 chars ext)
    uint8_t  DIR_Attr;          // Offset 11: Attribute byte
    uint8_t  DIR_NTRes;         // Offset 12: Reserved for Windows NT
    uint8_t  DIR_CrtTimeTenth;  // Offset 13: Creation time tenths of a second
    uint16_t DIR_CrtTime;       // Offset 14: Creation time
    uint16_t DIR_CrtDate;       // Offset 16: Creation date
    uint16_t DIR_LstAccDate;    // Offset 18: Last access date
    uint16_t DIR_FstClusHI;     // Offset 20: High word of starting cluster
    uint16_t DIR_WrtTime;       // Offset 22: Last write time
    uint16_t DIR_WrtDate;       // Offset 24: Last write date
    uint16_t DIR_FstClusLO;     // Offset 26: Low word of starting cluster
    uint32_t DIR_FileSize;      // Offset 28: File size in bytes
} __attribute__((packed)) fat32_dir_entry_t;

// Read Function Prototypes
uint32_t fat32_get_next_cluster(uint32_t cluster);
void fat32_list_directory(uint32_t dir_cluster);
int fat32_find_entry(uint32_t dir_cluster, const char *name, uint32_t *out_cluster, uint32_t *out_size, uint8_t *out_attr);
int fat32_read_file_content(uint32_t start_cluster, uint32_t file_size, uint8_t *out_buffer, uint32_t max_buffer_size);

// Write & Allocation Function Prototypes
int fat32_set_next_cluster(uint32_t cluster, uint32_t value);
uint32_t fat32_allocate_cluster(void);
int fat32_write_dir_entry(uint32_t dir_cluster, const char *short_name, uint8_t attr, uint32_t start_cluster, uint32_t file_size);
int fat32_write_file_content(uint32_t start_cluster, const uint8_t *data, uint32_t size);
int fat32_delete_entry(uint32_t dir_cluster, const char *name);

// Shell Directory Navigation Helper Functions
uint32_t fat32_get_current_dir_cluster(void);
const char* fat32_get_current_path(void);
int fat32_change_dir(const char *foldername);

#endif
