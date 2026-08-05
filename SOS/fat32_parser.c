#include "fat32_parser.h"
#include "usb_storage.h"
#include "screen.h"
#include "libc/stdio.h"
#include "libc/string.h"

static fat32_info_t fs_info;
static uint8_t mbr_sector_buffer[512] __attribute__((aligned(16)));
static uint8_t boot_sector_buffer[512] __attribute__((aligned(16)));

const fat32_info_t *fat32_get_info(void) {
    return &fs_info;
}

// Formula: LBA = First_Data_Sector + ((cluster - 2) * SectorsPerCluster)
uint32_t fat32_cluster_to_lba(uint32_t cluster) {
    if (!fs_info.is_mounted || cluster < 2) return 0;
    uint32_t lba = fs_info.first_data_sector + ((cluster - 2) * fs_info.sectors_per_cluster);
    printf("[DEBUG Cluster->LBA] Cluster %u -> Calculated LBA %u\n", cluster, lba);
    return lba;
}

// MBR Partition Scanner (Bug 1 Fix: Removed || selected_lba == 0 clause)
static int mbr_find_partition(uint8_t *mbr_sector, int target_partition_index, uint32_t *out_start_lba, uint8_t *out_type) {
    printf("[MBR Parser] Scanning MBR Partition Table at Offset 0x1BE...\n");

    mbr_partition_entry_t *entries = (mbr_partition_entry_t *)(mbr_sector + 0x1BE);
    int fat_partition_count = 0;
    uint32_t selected_lba = 0;
    uint32_t selected_sectors = 0;
    uint8_t selected_type = 0;

    for (int i = 0; i < 4; i++) {
        uint8_t type = entries[i].partition_type;
        uint32_t start_lba = entries[i].lba_start;
        uint32_t sectors = entries[i].total_sectors;

        if (type == 0x00 && sectors == 0) continue; // Unused partition entry

        printf("  Partition %d: Type 0x%02X ", i + 1, type);
        if (type == 0x0B || type == 0x0C) printf("(FAT32 LBA/CHS)");
        else if (type == 0x06 || type == 0x0E) printf("(FAT16 LBA/CHS)");
        else if (type == 0x83) printf("(Linux Native)");
        else if (type == 0x07) printf("(NTFS/exFAT)");
        else printf("(Unknown Type)");

        printf(", Start LBA: %u, Total Sectors: %u\n", start_lba, sectors);

        // Bug 1 Fix: Only select when fat_partition_count == target_partition_index (Removed || selected_lba == 0)
        if (type == 0x0B || type == 0x0C || type == 0x06 || type == 0x0E) {
            if (fat_partition_count == target_partition_index) {
                selected_lba = start_lba;
                selected_type = type;
                selected_sectors = sectors;
            }
            fat_partition_count++;
        }
    }

    if (selected_lba > 0) {
        if (out_start_lba) *out_start_lba = selected_lba;
        if (out_type) *out_type = selected_type;
        printf("[MBR Parser] Selected Partition (Index %d): Start LBA = %u, Total Sectors = %u, Type = 0x%02X\n",
               target_partition_index, selected_lba, selected_sectors, selected_type);
        return 1;
    }

    printf("[MBR Parser] Warning: Target FAT partition index %d not found, falling back to LBA 0.\n", target_partition_index);
    if (out_start_lba) *out_start_lba = 0;
    if (out_type) *out_type = 0;
    return 0;
}

void fat32_print_info(void) {
    if (!fs_info.is_mounted) {
        printf("[FAT32 Parser] Error: FAT32 Volume not mounted.\n");
        return;
    }

    uint32_t total_mb = (uint32_t)(((uint64_t)fs_info.total_sectors * fs_info.bytes_per_sector) / (1024 * 1024));
    uint32_t root_lba = fat32_cluster_to_lba(fs_info.root_cluster);
    uint32_t cluster_bytes = fs_info.sectors_per_cluster * fs_info.bytes_per_sector;

    printf("====================================================\n");
    printf("        FAT32 BIOS Parameter Block (BPB) Info       \n");
    printf("====================================================\n");
    printf("  Partition Start LBA   : LBA %u (MBR Offset)\n", fs_info.partition_start_lba);
    printf("  Bytes Per Sector      : %u bytes (Offset 11)\n", fs_info.bytes_per_sector);
    printf("  Sectors Per Cluster   : %u sectors (%u bytes/cluster) (Offset 13)\n",
           fs_info.sectors_per_cluster, cluster_bytes);
    printf("  Reserved Sectors      : %u sectors (Offset 14)\n", fs_info.reserved_sectors);
    printf("  Number of FATs        : %u (Offset 16)\n", fs_info.num_fats);
    printf("  Sectors Per FAT       : %u sectors (Offset 36)\n", fs_info.fat_size_sectors);
    printf("  First FAT LBA Sector  : LBA %u\n", fs_info.first_fat_sector);
    printf("  First Data LBA Sector : LBA %u\n", fs_info.first_data_sector);
    printf("  Root Cluster Number   : Cluster %u (Offset 44)\n", fs_info.root_cluster);
    printf("  Root Cluster LBA      : LBA %u\n", root_lba);
    printf("  Total Volume Sectors  : %u sectors (Offset 32)\n", fs_info.total_sectors);
    printf("  Total Volume Size     : %u MB\n", total_mb);
    printf("  Volume Label          : %s (Offset 71)\n", fs_info.volume_label);
    printf("  File System Type      : %s (Offset 82)\n", fs_info.fs_type);
    printf("====================================================\n");
}

int fat32_mount(void) {
    printf("[FAT32 Parser] Reading LBA Sector 0 (Master Boot Record / MBR)...\n");

    memset(&fs_info, 0, sizeof(fat32_info_t));
    memset(mbr_sector_buffer, 0, sizeof(mbr_sector_buffer));

    // 1. Read LBA Sector 0 (Real MBR)
    if (!usb_storage_read_sector(1, 0, mbr_sector_buffer)) {
        printf("[FAT32 Parser] Error: Failed to read Sector 0 (MBR) from USB drive.\n");
        return 0;
    }

    // Verify MBR Signature at offset 510-511 (0x55, 0xAA)
    uint16_t mbr_sig = ((uint16_t)mbr_sector_buffer[511] << 8) | mbr_sector_buffer[510];
    if (mbr_sig != 0xAA55) {
        printf("[FAT32 Parser] Error: Invalid MBR Boot Signature 0x%04X at offset 510.\n", mbr_sig);
        return 0;
    }

    // 2. Parse MBR Partition Table at offset 0x1BE to locate Partition 2 (Data Storage Partition)
    uint32_t partition_lba = 0;
    uint8_t partition_type = 0;

    // Per project design: Partition 1 = OS boot, Partition 2 = Data Storage Partition (index 1)
    mbr_find_partition(mbr_sector_buffer, 1, &partition_lba, &partition_type);

    // 3. Read the ACTUAL FAT32 Volume Boot Record (VBR / BPB) at partition_lba
    printf("[FAT32 Parser] Reading VBR / BPB Boot Sector at LBA %u...\n", partition_lba);
    memset(boot_sector_buffer, 0, sizeof(boot_sector_buffer));

    if (!usb_storage_read_sector(1, partition_lba, boot_sector_buffer)) {
        printf("[FAT32 Parser] Error: Failed to read VBR Boot Sector at LBA %u.\n", partition_lba);
        return 0;
    }

    // Verify VBR Boot Signature at offset 510-511
    uint16_t vbr_sig = ((uint16_t)boot_sector_buffer[511] << 8) | boot_sector_buffer[510];
    if (vbr_sig != 0xAA55) {
        printf("[FAT32 Parser] Error: Invalid VBR Signature 0x%04X at LBA %u offset 510.\n", vbr_sig, partition_lba);
        return 0;
    }

    bpb_fat32_t *bpb = (bpb_fat32_t *)boot_sector_buffer;

    // 4. Parse true BPB Fields into Struct
    fs_info.partition_start_lba = partition_lba;
    fs_info.bytes_per_sector = bpb->bytes_per_sector;
    fs_info.sectors_per_cluster = bpb->sectors_per_cluster;
    fs_info.reserved_sectors = bpb->reserved_sectors;
    fs_info.num_fats = bpb->num_fats;
    fs_info.total_sectors = bpb->total_sectors_32;
    fs_info.fat_size_sectors = bpb->fat_size_32;
    fs_info.root_cluster = bpb->root_cluster;

    // Calculate Partition-Relative LBA offsets
    fs_info.first_fat_sector = partition_lba + bpb->reserved_sectors;
    fs_info.first_data_sector = partition_lba + bpb->reserved_sectors + (bpb->num_fats * bpb->fat_size_32);

    memcpy(fs_info.volume_label, bpb->volume_label, 11);
    fs_info.volume_label[11] = '\0';

    memcpy(fs_info.fs_type, bpb->file_system_type, 8);
    fs_info.fs_type[8] = '\0';

    fs_info.is_mounted = 1;
    printf("[FAT32 Parser] FAT32 Volume at LBA %u Mounted Successfully!\n", partition_lba);

    // Print Parsed BPB Details
    fat32_print_info();

    return 1;
}
