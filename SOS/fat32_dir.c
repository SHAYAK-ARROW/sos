#include "fat32_dir.h"
#include "fat32_parser.h"
#include "usb_storage.h"
#include "screen.h"
#include "libc/stdio.h"
#include "libc/string.h"

#define MAX_DIR_DEPTH 16

static uint32_t current_dir_cluster = 0;
static char current_path[64] = "/";

// Directory Stack for cd .. Parent Navigation
static uint32_t dir_cluster_stack[MAX_DIR_DEPTH];
static char dir_name_stack[MAX_DIR_DEPTH][32];
static int dir_stack_depth = 0;

static uint8_t sector_buffer[512] __attribute__((aligned(16)));
static uint8_t fat_sector_buffer[512] __attribute__((aligned(16)));
static uint8_t zero_sector_buffer[512] __attribute__((aligned(16)));

// FAT Sector Cache for fat32_allocate_cluster()
static uint8_t cached_fat_sector[512] __attribute__((aligned(16)));
static uint32_t cached_sector_num = 0xFFFFFFFF;
static uint32_t last_free_cluster_hint = 2;

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

// Format a filename string into FAT 8.3 space-padded DIR_Name (11 bytes)
static void fat32_format_83_name(const char *input_name, char *out_83) {
    memset(out_83, ' ', 11);
    
    // Check FAT32 spec 0xE5 substitution rule (if first char is 0x05, store as 0x05)
    char first_char = input_name[0];
    if ((uint8_t)first_char == 0x05) {
        out_83[0] = 0x05;
        input_name++;
    }

    const char *dot = strchr(input_name, '.');
    int name_len = dot ? (int)(dot - input_name) : (int)strlen(input_name);
    if (name_len > 8) name_len = 8;

    for (int i = 0; i < name_len; i++) {
        char c = input_name[i];
        if (c >= 'a' && c <= 'z') c -= 32; // Uppercase
        out_83[i] = c;
    }

    if (dot) {
        const char *ext = dot + 1;
        int ext_len = (int)strlen(ext);
        if (ext_len > 3) ext_len = 3;
        for (int i = 0; i < ext_len; i++) {
            char c = ext[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            out_83[8 + i] = c;
        }
    }
}

// Convert 8.3 FAT32 DIR_Name string to standard display string
static void fat32_format_short_name(const char *raw_name, uint8_t attr, char *out_name) {
    char name[9];
    char ext[4];
    int name_idx = 0;
    int ext_idx = 0;

    int start_i = 0;
    if ((uint8_t)raw_name[0] == 0x05) {
        name[name_idx++] = (char)0xE5;
        start_i = 1;
    }

    for (int i = start_i; i < 8; i++) {
        if (raw_name[i] != ' ') {
            name[name_idx++] = raw_name[i];
        }
    }
    name[name_idx] = '\0';

    for (int i = 8; i < 11; i++) {
        if (raw_name[i] != ' ') {
            ext[ext_idx++] = raw_name[i];
        }
    }
    ext[ext_idx] = '\0';

    if (attr & FAT32_ATTR_DIRECTORY) {
        strcpy(out_name, name);
    } else {
        strcpy(out_name, name);
        if (ext_idx > 0) {
            strcat(out_name, ".");
            strcat(out_name, ext);
        }
    }
}

// 1. Read Next Cluster in Chain from FAT Table
uint32_t fat32_get_next_cluster(uint32_t cluster) {
    const fat32_info_t *info = fat32_get_info();
    if (!info || !info->is_mounted || cluster < 2) return 0x0FFFFFFF;

    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = info->first_fat_sector + (fat_offset / info->bytes_per_sector);
    uint32_t entry_offset = fat_offset % info->bytes_per_sector;

    if (!usb_storage_read_sector(1, fat_sector, fat_sector_buffer)) {
        printf("[FAT32 Dir] Error: Failed to read FAT table sector %u.\n", fat_sector);
        return 0x0FFFFFFF;
    }

    uint32_t next_cluster = *(uint32_t *)&fat_sector_buffer[entry_offset] & 0x0FFFFFFF;

    if (next_cluster >= 0x0FFFFFF8) {
        return 0x0FFFFFFF; // End of chain
    }

    return next_cluster;
}

// 2a. Write Next Cluster in FAT Table across ALL FAT copies
int fat32_set_next_cluster(uint32_t cluster, uint32_t value) {
    const fat32_info_t *info = fat32_get_info();
    if (!info || !info->is_mounted || cluster < 2) return 0;

    uint32_t fat_offset = cluster * 4;

    // Update ALL FAT copies (info->num_fats)
    for (uint8_t f = 0; f < info->num_fats; f++) {
        uint32_t fat_start = info->first_fat_sector + (f * info->fat_size_sectors);
        uint32_t fat_sector = fat_start + (fat_offset / info->bytes_per_sector);
        uint32_t entry_offset = fat_offset % info->bytes_per_sector;

        if (!usb_storage_read_sector(1, fat_sector, fat_sector_buffer)) {
            printf("[FAT32 Dir] Error: Failed to read FAT%d sector %u.\n", f + 1, fat_sector);
            return 0;
        }

        uint32_t orig = *(uint32_t *)&fat_sector_buffer[entry_offset];
        *(uint32_t *)&fat_sector_buffer[entry_offset] = (orig & 0xF0000000) | (value & 0x0FFFFFFF);

        if (!usb_storage_write_sector(1, fat_sector, fat_sector_buffer)) {
            printf("[FAT32 Dir] Error: Failed to write FAT%d sector %u.\n", f + 1, fat_sector);
            return 0;
        }
    }

    // Invalidate allocation sector cache on write
    cached_sector_num = 0xFFFFFFFF;

    return 1;
}

// 2b. High-Performance FAT Sector Cached Cluster Allocator
uint32_t fat32_allocate_cluster(void) {
    const fat32_info_t *info = fat32_get_info();
    if (!info || !info->is_mounted) return 0;

    uint32_t total_clusters = (info->total_sectors - (info->first_data_sector - info->partition_start_lba)) / info->sectors_per_cluster;
    uint32_t start_clus = last_free_cluster_hint;
    if (start_clus < 2 || start_clus >= total_clusters + 2) {
        start_clus = 2;
    }

    // Pass 1: Scan from last_free_cluster_hint to total_clusters + 1
    // Pass 2: Wrap around from cluster 2 to last_free_cluster_hint if needed
    for (int pass = 0; pass < 2; pass++) {
        uint32_t search_start = (pass == 0) ? start_clus : 2;
        uint32_t search_end = (pass == 0) ? (total_clusters + 2) : start_clus;

        for (uint32_t clus = search_start; clus < search_end; clus++) {
            uint32_t fat_offset = clus * 4;
            uint32_t fat_sector = info->first_fat_sector + (fat_offset / info->bytes_per_sector);
            uint32_t entry_offset = fat_offset % info->bytes_per_sector;

            // Only read USB disk sector if not already cached in memory!
            if (fat_sector != cached_sector_num) {
                if (!usb_storage_read_sector(1, fat_sector, cached_fat_sector)) {
                    printf("[FAT32 Dir] Error: Failed to read FAT sector %u for allocation.\n", fat_sector);
                    return 0;
                }
                cached_sector_num = fat_sector;
            }

            uint32_t entry_val = *(uint32_t *)&cached_fat_sector[entry_offset] & 0x0FFFFFFF;

            if (entry_val == 0x00000000) { // Free cluster found!
                last_free_cluster_hint = clus + 1; // Update allocation hint

                // Mark cluster as End-of-Chain (0x0FFFFFFF)
                if (!fat32_set_next_cluster(clus, 0x0FFFFFFF)) {
                    return 0;
                }

                // Zero out all sectors on physical disk for this newly allocated cluster
                uint32_t start_lba = fat32_cluster_to_lba(clus);
                memset(zero_sector_buffer, 0, 512);

                for (uint32_t sec = 0; sec < info->sectors_per_cluster; sec++) {
                    usb_storage_write_sector(1, start_lba + sec, zero_sector_buffer);
                }

                return clus;
            }
        }
    }

    printf("[FAT32 Dir] Error: USB Storage is full (No free FAT clusters).\n");
    return 0;
}

// 2c. Find Free Directory Entry Slot (0x00 or 0xE5) & Write 32-Byte Entry
int fat32_write_dir_entry(uint32_t dir_cluster, const char *short_name, uint8_t attr, uint32_t start_cluster, uint32_t file_size) {
    const fat32_info_t *info = fat32_get_info();
    if (!info || !info->is_mounted) return 0;

    if (dir_cluster < 2) dir_cluster = info->root_cluster;

    char formatted_83[11];
    fat32_format_83_name(short_name, formatted_83);

    uint32_t curr_cluster = dir_cluster;
    uint32_t last_cluster = dir_cluster;

    while (curr_cluster >= 2 && curr_cluster < 0x0FFFFFF8) {
        last_cluster = curr_cluster;
        uint32_t start_lba = fat32_cluster_to_lba(curr_cluster);

        for (uint32_t sec = 0; sec < info->sectors_per_cluster; sec++) {
            uint32_t lba = start_lba + sec;
            if (!usb_storage_read_sector(1, lba, sector_buffer)) break;

            for (int entry_idx = 0; entry_idx < 16; entry_idx++) {
                fat32_dir_entry_t *entry = (fat32_dir_entry_t *)&sector_buffer[entry_idx * 32];

                // Free slot found (0x00 = Unused end, 0xE5 = Deleted)
                if ((uint8_t)entry->DIR_Name[0] == 0x00 || (uint8_t)entry->DIR_Name[0] == 0xE5) {
                    memcpy(entry->DIR_Name, formatted_83, 11);
                    entry->DIR_Attr = attr;
                    entry->DIR_NTRes = 0;
                    entry->DIR_CrtTimeTenth = 0;
                    entry->DIR_CrtTime = 0x4800;
                    entry->DIR_CrtDate = 0x5821;
                    entry->DIR_LstAccDate = 0x5821;
                    entry->DIR_FstClusHI = (uint16_t)((start_cluster >> 16) & 0xFFFF);
                    entry->DIR_WrtTime = 0x4800;
                    entry->DIR_WrtDate = 0x5821;
                    entry->DIR_FstClusLO = (uint16_t)(start_cluster & 0xFFFF);
                    entry->DIR_FileSize = file_size;

                    if (!usb_storage_write_sector(1, lba, sector_buffer)) {
                        printf("[FAT32 Dir] Error: Failed to write directory entry sector %u.\n", lba);
                        return 0;
                    }

                    return 1; // Success!
                }
            }
        }

        curr_cluster = fat32_get_next_cluster(curr_cluster);
    }

    // Directory cluster full -> Allocate new directory cluster and chain
    uint32_t new_dir_cluster = fat32_allocate_cluster();
    if (new_dir_cluster == 0) return 0;

    fat32_set_next_cluster(last_cluster, new_dir_cluster);

    // Write directory entry into first slot of new cluster
    uint32_t new_lba = fat32_cluster_to_lba(new_dir_cluster);
    memset(sector_buffer, 0, 512);

    fat32_dir_entry_t *entry = (fat32_dir_entry_t *)&sector_buffer[0];
    memcpy(entry->DIR_Name, formatted_83, 11);
    entry->DIR_Attr = attr;
    entry->DIR_FstClusHI = (uint16_t)((start_cluster >> 16) & 0xFFFF);
    entry->DIR_FstClusLO = (uint16_t)(start_cluster & 0xFFFF);
    entry->DIR_FileSize = file_size;

    return usb_storage_write_sector(1, new_lba, sector_buffer);
}

// 2d. Write Data Payload across Cluster Chain
int fat32_write_file_content(uint32_t start_cluster, const uint8_t *data, uint32_t size) {
    const fat32_info_t *info = fat32_get_info();
    if (!info || !info->is_mounted || start_cluster < 2) return 0;

    uint32_t curr_cluster = start_cluster;
    uint32_t bytes_written = 0;

    while (bytes_written < size) {
        uint32_t start_lba = fat32_cluster_to_lba(curr_cluster);

        for (uint32_t sec = 0; sec < info->sectors_per_cluster; sec++) {
            uint32_t lba = start_lba + sec;
            memset(sector_buffer, 0, 512);

            uint32_t chunk = 512;
            if (bytes_written + chunk > size) chunk = size - bytes_written;

            if (chunk > 0) {
                memcpy(sector_buffer, data + bytes_written, chunk);
                bytes_written += chunk;
            }

            if (!usb_storage_write_sector(1, lba, sector_buffer)) {
                printf("[FAT32 Dir] Error: Failed to write file data sector %u.\n", lba);
                return 0;
            }

            if (bytes_written >= size) break;
        }

        if (bytes_written < size) {
            uint32_t next = fat32_get_next_cluster(curr_cluster);
            if (next >= 0x0FFFFFF8) {
                uint32_t new_clus = fat32_allocate_cluster();
                if (new_clus == 0) return 0;
                fat32_set_next_cluster(curr_cluster, new_clus);
                curr_cluster = new_clus;
            } else {
                curr_cluster = next;
            }
        }
    }

    return 1;
}

// 2e. Delete File/Directory Entry & Free Entire Cluster Chain
int fat32_delete_entry(uint32_t dir_cluster, const char *name) {
    const fat32_info_t *info = fat32_get_info();
    if (!info || !info->is_mounted) return 0;

    if (dir_cluster < 2) dir_cluster = info->root_cluster;

    uint32_t curr_cluster = dir_cluster;

    while (curr_cluster >= 2 && curr_cluster < 0x0FFFFFF8) {
        uint32_t start_lba = fat32_cluster_to_lba(curr_cluster);

        for (uint32_t sec = 0; sec < info->sectors_per_cluster; sec++) {
            uint32_t lba = start_lba + sec;
            if (!usb_storage_read_sector(1, lba, sector_buffer)) break;

            for (int entry_idx = 0; entry_idx < 16; entry_idx++) {
                fat32_dir_entry_t *entry = (fat32_dir_entry_t *)&sector_buffer[entry_idx * 32];

                if ((uint8_t)entry->DIR_Name[0] == 0x00) return 0;
                if ((uint8_t)entry->DIR_Name[0] == 0xE5) continue;
                if (entry->DIR_Attr == FAT32_ATTR_LFN || (entry->DIR_Attr & FAT32_ATTR_VOLUME_ID)) continue;

                char formatted_name[32];
                fat32_format_short_name(entry->DIR_Name, entry->DIR_Attr, formatted_name);

                if (strcasecmp_local(formatted_name, name) == 0) {
                    uint32_t target_cluster = ((uint32_t)entry->DIR_FstClusHI << 16) | entry->DIR_FstClusLO;

                    // Mark directory entry as deleted (0xE5)
                    entry->DIR_Name[0] = (char)0xE5;
                    usb_storage_write_sector(1, lba, sector_buffer);

                    // Free entire cluster chain in FAT table
                    uint32_t c = target_cluster;
                    while (c >= 2 && c < 0x0FFFFFF8) {
                        uint32_t next = fat32_get_next_cluster(c);
                        fat32_set_next_cluster(c, 0x00000000); // Mark free
                        c = next;
                    }

                    printf("[FAT32 Dir] Deleted '%s' and freed cluster chain starting at %u.\n", name, target_cluster);
                    return 1;
                }
            }
        }

        curr_cluster = fat32_get_next_cluster(curr_cluster);
    }

    return 0;
}

// 3. List files/folders in directory cluster
void fat32_list_directory(uint32_t dir_cluster) {
    const fat32_info_t *info = fat32_get_info();
    if (!info || !info->is_mounted) {
        printf("[FAT32 Dir] Error: Volume not mounted.\n");
        return;
    }

    if (dir_cluster < 2) {
        dir_cluster = info->root_cluster;
    }

    printf("Directory listing of %s (Cluster %u):\n", current_path, dir_cluster);

    uint32_t curr_cluster = dir_cluster;
    int count = 0;

    while (curr_cluster >= 2 && curr_cluster < 0x0FFFFFF8) {
        uint32_t start_lba = fat32_cluster_to_lba(curr_cluster);

        for (uint32_t sec = 0; sec < info->sectors_per_cluster; sec++) {
            uint32_t lba = start_lba + sec;
            if (!usb_storage_read_sector(1, lba, sector_buffer)) break;

            for (int entry_idx = 0; entry_idx < 16; entry_idx++) {
                fat32_dir_entry_t *entry = (fat32_dir_entry_t *)&sector_buffer[entry_idx * 32];

                if ((uint8_t)entry->DIR_Name[0] == 0x00) {
                    if (count == 0) printf("  (directory is empty)\n");
                    return;
                }

                if ((uint8_t)entry->DIR_Name[0] == 0xE5) continue;
                if (entry->DIR_Attr == FAT32_ATTR_LFN || (entry->DIR_Attr & FAT32_ATTR_VOLUME_ID)) continue;

                char formatted_name[32];
                fat32_format_short_name(entry->DIR_Name, entry->DIR_Attr, formatted_name);

                count++;
                if (entry->DIR_Attr & FAT32_ATTR_DIRECTORY) {
                    printf("  [DIR ]  %s/\n", formatted_name);
                } else {
                    printf("  [FILE]  %s    (%u bytes)\n", formatted_name, entry->DIR_FileSize);
                }
            }
        }

        curr_cluster = fat32_get_next_cluster(curr_cluster);
    }

    if (count == 0) {
        printf("  (directory is empty)\n");
    }
}

// 4. Find entry matching filename/foldername in dir_cluster
int fat32_find_entry(uint32_t dir_cluster, const char *name, uint32_t *out_cluster, uint32_t *out_size, uint8_t *out_attr) {
    const fat32_info_t *info = fat32_get_info();
    if (!info || !info->is_mounted) return 0;

    if (dir_cluster < 2) dir_cluster = info->root_cluster;

    uint32_t curr_cluster = dir_cluster;

    while (curr_cluster >= 2 && curr_cluster < 0x0FFFFFF8) {
        uint32_t start_lba = fat32_cluster_to_lba(curr_cluster);

        for (uint32_t sec = 0; sec < info->sectors_per_cluster; sec++) {
            uint32_t lba = start_lba + sec;
            if (!usb_storage_read_sector(1, lba, sector_buffer)) break;

            for (int entry_idx = 0; entry_idx < 16; entry_idx++) {
                fat32_dir_entry_t *entry = (fat32_dir_entry_t *)&sector_buffer[entry_idx * 32];

                if ((uint8_t)entry->DIR_Name[0] == 0x00) return 0;
                if ((uint8_t)entry->DIR_Name[0] == 0xE5) continue;
                if (entry->DIR_Attr == FAT32_ATTR_LFN || (entry->DIR_Attr & FAT32_ATTR_VOLUME_ID)) continue;

                char formatted_name[32];
                fat32_format_short_name(entry->DIR_Name, entry->DIR_Attr, formatted_name);

                if (strcasecmp_local(formatted_name, name) == 0) {
                    uint32_t start_clus = ((uint32_t)entry->DIR_FstClusHI << 16) | entry->DIR_FstClusLO;
                    if (out_cluster) *out_cluster = start_clus;
                    if (out_size) *out_size = entry->DIR_FileSize;
                    if (out_attr) *out_attr = entry->DIR_Attr;
                    return 1;
                }
            }
        }

        curr_cluster = fat32_get_next_cluster(curr_cluster);
    }

    return 0;
}

// 5. Read entire file content across cluster chain
int fat32_read_file_content(uint32_t start_cluster, uint32_t file_size, uint8_t *out_buffer, uint32_t max_buffer_size) {
    const fat32_info_t *info = fat32_get_info();
    if (!info || !info->is_mounted || start_cluster < 2) return 0;

    uint32_t curr_cluster = start_cluster;
    uint32_t bytes_read = 0;

    while (curr_cluster >= 2 && curr_cluster < 0x0FFFFFF8 && bytes_read < file_size && bytes_read < max_buffer_size) {
        uint32_t start_lba = fat32_cluster_to_lba(curr_cluster);

        for (uint32_t sec = 0; sec < info->sectors_per_cluster; sec++) {
            uint32_t lba = start_lba + sec;
            if (!usb_storage_read_sector(1, lba, sector_buffer)) break;

            uint32_t chunk = info->bytes_per_sector;
            if (bytes_read + chunk > file_size) chunk = file_size - bytes_read;
            if (bytes_read + chunk > max_buffer_size) chunk = max_buffer_size - bytes_read;

            memcpy(out_buffer + bytes_read, sector_buffer, chunk);
            bytes_read += chunk;

            if (bytes_read >= file_size || bytes_read >= max_buffer_size) break;
        }

        curr_cluster = fat32_get_next_cluster(curr_cluster);
    }

    if (bytes_read < max_buffer_size) {
        out_buffer[bytes_read] = '\0';
    }

    return 1;
}

uint32_t fat32_get_current_dir_cluster(void) {
    const fat32_info_t *info = fat32_get_info();
    if (current_dir_cluster < 2 && info && info->is_mounted) {
        current_dir_cluster = info->root_cluster;
    }
    return current_dir_cluster;
}

const char* fat32_get_current_path(void) {
    return current_path;
}

// Stack-based cd & Directory Navigation with Path Overflow Bounds Check
int fat32_change_dir(const char *foldername) {
    const fat32_info_t *info = fat32_get_info();
    if (!info || !info->is_mounted) return 0;

    if (current_dir_cluster < 2) {
        current_dir_cluster = info->root_cluster;
    }

    // cd / or cd (reset to root)
    if (strcmp(foldername, "/") == 0 || foldername[0] == '\0') {
        current_dir_cluster = info->root_cluster;
        dir_stack_depth = 0;
        strcpy(current_path, "/");
        printf("Changed directory to /\n");
        return 1;
    }

    // cd .. (navigate to immediate parent using dir_stack)
    if (strcmp(foldername, "..") == 0) {
        if (dir_stack_depth > 0) {
            dir_stack_depth--;
            current_dir_cluster = dir_cluster_stack[dir_stack_depth];

            // Reconstruct path string
            strcpy(current_path, "/");
            for (int i = 0; i < dir_stack_depth; i++) {
                if (strlen(current_path) + strlen(dir_name_stack[i]) + 2 < 64) {
                    if (i > 0) strcat(current_path, "/");
                    strcat(current_path, dir_name_stack[i]);
                }
            }
        } else {
            current_dir_cluster = info->root_cluster;
            strcpy(current_path, "/");
        }
        printf("Changed directory to %s\n", current_path);
        return 1;
    }

    uint32_t target_cluster = 0;
    uint32_t target_size = 0;
    uint8_t target_attr = 0;

    if (fat32_find_entry(current_dir_cluster, foldername, &target_cluster, &target_size, &target_attr)) {
        if (target_attr & FAT32_ATTR_DIRECTORY) {
            if (dir_stack_depth < MAX_DIR_DEPTH) {
                dir_cluster_stack[dir_stack_depth] = current_dir_cluster;
                strncpy(dir_name_stack[dir_stack_depth], foldername, 31);
                dir_name_stack[dir_stack_depth][31] = '\0';
                dir_stack_depth++;
            }

            current_dir_cluster = target_cluster;

            // Bounds check before strcat to prevent overflow of 64-byte current_path buffer
            if (strlen(current_path) + strlen(foldername) + 2 < 64) {
                if (strcmp(current_path, "/") != 0) strcat(current_path, "/");
                strcat(current_path, foldername);
            }

            printf("Changed directory to %s (Cluster %u)\n", current_path, current_dir_cluster);
            return 1;
        } else {
            printf("Error: '%s' is a file, not a directory.\n", foldername);
            return 0;
        }
    }

    printf("Error: Directory '%s' not found.\n", foldername);
    return 0;
}
