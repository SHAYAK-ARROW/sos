#ifndef FAT32_PARSER_H
#define FAT32_PARSER_H

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// MBR Partition Table Entry (16 Bytes at offset 0x1BE in Sector 0)
// ============================================================================
typedef struct {
    uint8_t  boot_indicator;   // 0x80 = Active/Bootable, 0x00 = Inactive
    uint8_t  starting_head;    // CHS starting head
    uint8_t  starting_sector;  // CHS starting sector/cylinder
    uint8_t  starting_cylinder;// CHS starting cylinder
    uint8_t  partition_type;   // System ID (0x0B/0x0C = FAT32, 0x06/0x0E = FAT16)
    uint8_t  ending_head;      // CHS ending head
    uint8_t  ending_sector;    // CHS ending sector/cylinder
    uint8_t  ending_cylinder;  // CHS ending cylinder
    uint32_t lba_start;        // Starting LBA sector number
    uint32_t total_sectors;    // Total sector count
} __attribute__((packed)) mbr_partition_entry_t;

// ============================================================================
// FAT32 BIOS Parameter Block (BPB) Structure
// ============================================================================
typedef struct {
    uint8_t  jmpBoot[3];          // Offset 0: Jump instruction to boot code
    char     OEMName[8];          // Offset 3: OEM Name (e.g. "MSWIN4.1")
    uint16_t bytes_per_sector;    // Offset 11: Bytes per sector (512)
    uint8_t  sectors_per_cluster; // Offset 13: Sectors per cluster (1, 8, 32)
    uint16_t reserved_sectors;    // Offset 14: Reserved sectors count (32)
    uint8_t  num_fats;            // Offset 16: Number of FATs (2)
    uint16_t root_entry_count;    // Offset 17: Must be 0 for FAT32
    uint16_t total_sectors_16;    // Offset 19: Must be 0 for FAT32
    uint8_t  media_type;          // Offset 21: Media descriptor (0xF8)
    uint16_t fat_size_16;         // Offset 22: Must be 0 for FAT32
    uint16_t sectors_per_track;   // Offset 24: Sectors per track
    uint16_t num_heads;           // Offset 26: Number of heads
    uint32_t hidden_sectors;      // Offset 28: Count of hidden sectors
    uint32_t total_sectors_32;    // Offset 32: Total count of sectors on volume

    // FAT32 Extended Fields (Offset 36..89)
    uint32_t fat_size_32;         // Offset 36: Sectors occupied by ONE FAT
    uint16_t ext_flags;           // Offset 40: Extended flags
    uint16_t fs_version;          // Offset 42: File system version (0x0000)
    uint32_t root_cluster;        // Offset 44: Cluster number of Root Directory (2)
    uint16_t fs_info_sector;      // Offset 48: Sector number of FSINFO structure
    uint16_t backup_boot_sector;  // Offset 50: Sector number of backup boot sector
    uint8_t  reserved[12];        // Offset 52: Reserved
    uint8_t  drive_number;        // Offset 64: Physical drive number
    uint8_t  reserved1;           // Offset 65: Reserved
    uint8_t  boot_signature;      // Offset 66: Extended boot signature (0x29)
    uint32_t volume_id;           // Offset 67: Volume serial number
    char     volume_label[11];    // Offset 71: Volume label string
    char     file_system_type[8]; // Offset 82: "FAT32   " string
} __attribute__((packed)) bpb_fat32_t;

// Global FAT32 Mount Information Struct
typedef struct {
    uint32_t partition_start_lba; // Base LBA Sector offset for mounted partition
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint32_t total_sectors;
    uint32_t fat_size_sectors;
    uint32_t root_cluster;
    uint32_t first_fat_sector;
    uint32_t first_data_sector;
    char     volume_label[12];
    char     fs_type[9];
    int      is_mounted;
} fat32_info_t;

// Function Prototypes
int fat32_mount(void);
void fat32_print_info(void);
uint32_t fat32_cluster_to_lba(uint32_t cluster);
const fat32_info_t *fat32_get_info(void);

#endif
