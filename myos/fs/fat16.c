#include "fat16.h"
#include "../drivers/disk.h"
#include <stdint.h>

/* ===== Simple flat FAT16-like filesystem =====
 * Layout on disk (512 bytes per sector):
 * Sector 0:     Superblock / signature
 * Sector 1-4:   File Allocation Table (FAT) - 4 sectors = 2048 entries
 * Sector 5-36:  Directory entries (32 sectors, 16 entries/sector = 512 entries)
 * Sector 37+:   Data clusters (each cluster = 4 sectors = 2KB)
 *
 * Max files: 512
 * Max file size: limited by clusters
 * Total capacity: depends on disk size
 */

#define SECTOR_SIZE     512
#define SECTORS_PER_CLUSTER 4
#define CLUSTER_SIZE    (SECTOR_SIZE * SECTORS_PER_CLUSTER)

#define SB_SECTOR       0
#define FAT_START       1
#define FAT_SECTORS     4
#define DIR_START       5
#define DIR_SECTORS     32
#define DATA_START      37

#define DIR_ENTRIES_PER_SECTOR  (SECTOR_SIZE / 64)
#define MAX_DIR_ENTRIES         (DIR_SECTORS * DIR_ENTRIES_PER_SECTOR)
#define MAX_CLUSTERS            2048

#define SOS_MAGIC   0x534F5321  /* 'SOS!' */

/* Superblock */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t total_clusters;
    uint32_t free_clusters;
    uint8_t  pad[SECTOR_SIZE - 16];
} __attribute__((packed)) Superblock;

/* Directory entry (64 bytes) */
#define DIRENT_FREE     0x00
#define DIRENT_DELETED  0xE5
#define DIRENT_USED     0x01
#define DIRENT_DIR      0x10

typedef struct {
    uint8_t  status;        /* DIRENT_FREE/USED/DELETED/DIR */
    uint8_t  pad1[3];
    char     name[48];      /* filename with extension */
    uint32_t start_cluster;
    uint32_t size;
    uint8_t  pad2[4];
} __attribute__((packed)) DirEntry;  /* 64 bytes total */

/* ===== Helpers ===== */
static int f_strlen(const char* s){ int i=0; while(s[i]) i++; return i; }
static int f_strcmp(const char* a, const char* b){ while(*a&&*b&&*a==*b){a++;b++;} return *a-*b; }
static void f_strcpy(char* d, const char* s){ int i=0; while((d[i]=s[i])) i++; }
static void f_memset(void* p, uint8_t v, uint32_t n){ uint8_t* b=(uint8_t*)p; for(uint32_t i=0;i<n;i++) b[i]=v; }
static void f_memcpy(void* d, const void* s, uint32_t n){ uint8_t* dd=(uint8_t*)d; const uint8_t* ss=(const uint8_t*)s; for(uint32_t i=0;i<n;i++) dd[i]=ss[i]; }

static uint8_t sector_buf[SECTOR_SIZE];
static uint8_t fat_buf[FAT_SECTORS * SECTOR_SIZE];  /* 2KB FAT cache */
static int     fat_dirty = 0;
static int     fs_ready  = 0;

/* ===== FAT access ===== */
static uint16_t fat_get(uint16_t cluster){
    uint16_t* fat = (uint16_t*)fat_buf;
    return fat[cluster];
}
static void fat_set(uint16_t cluster, uint16_t val){
    uint16_t* fat = (uint16_t*)fat_buf;
    fat[cluster] = val;
    fat_dirty = 1;
}
static void fat_flush(void){
    if(!fat_dirty) return;
    for(int i=0;i<FAT_SECTORS;i++)
        disk_write_sector(FAT_START+i, fat_buf + i*SECTOR_SIZE);
    fat_dirty=0;
}
static uint16_t fat_alloc(void){
    uint16_t* fat=(uint16_t*)fat_buf;
    for(uint16_t i=2;i<MAX_CLUSTERS;i++){
        if(fat[i]==0x0000){ fat[i]=0xFFFF; fat_dirty=1; return i; }
    }
    return 0xFFFF; /* no space */
}
static void fat_free_chain(uint16_t cluster){
    while(cluster < 0xFFF8 && cluster >= 2){
        uint16_t next = fat_get(cluster);
        fat_set(cluster, 0x0000);
        cluster = next;
    }
}

/* ===== Sector I/O ===== */
static uint32_t cluster_to_sector(uint16_t cluster){
    return DATA_START + (uint32_t)(cluster-2) * SECTORS_PER_CLUSTER;
}

/* ===== Directory ===== */
static int dir_read(uint32_t idx, DirEntry* e){
    uint32_t sector = DIR_START + idx / DIR_ENTRIES_PER_SECTOR;
    uint32_t offset = idx % DIR_ENTRIES_PER_SECTOR;
    if(disk_read_sector(sector, sector_buf) != 0) return -1;
    f_memcpy(e, sector_buf + offset * sizeof(DirEntry), sizeof(DirEntry));
    return 0;
}
static int dir_write(uint32_t idx, DirEntry* e){
    uint32_t sector = DIR_START + idx / DIR_ENTRIES_PER_SECTOR;
    uint32_t offset = idx % DIR_ENTRIES_PER_SECTOR;
    if(disk_read_sector(sector, sector_buf) != 0) return -1;
    f_memcpy(sector_buf + offset * sizeof(DirEntry), e, sizeof(DirEntry));
    return disk_write_sector(sector, sector_buf);
}
static int dir_find(const char* name){
    DirEntry e;
    for(int i=0;i<MAX_DIR_ENTRIES;i++){
        if(dir_read(i,&e)!=0) continue;
        if(e.status==DIRENT_USED || e.status==DIRENT_DIR)
            if(f_strcmp(e.name, name)==0) return i;
    }
    return -1;
}
static int dir_alloc(void){
    DirEntry e;
    for(int i=0;i<MAX_DIR_ENTRIES;i++){
        if(dir_read(i,&e)!=0) continue;
        if(e.status==DIRENT_FREE || e.status==DIRENT_DELETED) return i;
    }
    return -1;
}

/* ===== Format ===== */
static int fat_format(void){
    /* write superblock */
    Superblock sb;
    f_memset(&sb, 0, sizeof(sb));
    sb.magic = SOS_MAGIC;
    sb.version = 1;
    sb.total_clusters = MAX_CLUSTERS;
    sb.free_clusters  = MAX_CLUSTERS - 2;
    f_memcpy(sector_buf, &sb, sizeof(sb));
    if(disk_write_sector(SB_SECTOR, sector_buf)!=0) return -1;

    /* clear FAT */
    f_memset(fat_buf, 0, sizeof(fat_buf));
    uint16_t* fat=(uint16_t*)fat_buf;
    fat[0]=0xFFF8; fat[1]=0xFFFF; /* reserved */
    for(int i=0;i<FAT_SECTORS;i++)
        if(disk_write_sector(FAT_START+i, fat_buf+i*SECTOR_SIZE)!=0) return -1;

    /* clear directory */
    f_memset(sector_buf, 0, SECTOR_SIZE);
    for(int i=0;i<DIR_SECTORS;i++)
        if(disk_write_sector(DIR_START+i, sector_buf)!=0) return -1;

    return 0;
}

/* ===== Init ===== */
int fat_init(void){
    fs_ready=0;
    if(!disk_present()) return -1;

    /* read superblock */
    if(disk_read_sector(SB_SECTOR, sector_buf)!=0) return -1;
    Superblock* sb=(Superblock*)sector_buf;

    if(sb->magic != SOS_MAGIC){
        /* not formatted — format now */
        if(fat_format()!=0) return -1;
    }

    /* load FAT into cache */
    for(int i=0;i<FAT_SECTORS;i++){
        if(disk_read_sector(FAT_START+i, fat_buf+i*SECTOR_SIZE)!=0) return -1;
    }
    fs_ready=1;
    return 0;
}

int fat_ready(void){ return fs_ready; }

/* ===== Read file ===== */
int fat_read(const char* path, char* buf, uint32_t maxlen){
    if(!fs_ready) return -1;
    int idx = dir_find(path);
    if(idx<0) return -1;
    DirEntry e; dir_read(idx,&e);
    if(e.status!=DIRENT_USED) return -1;

    uint32_t remaining = e.size < maxlen ? e.size : maxlen-1;
    uint32_t pos=0;
    uint16_t cluster=e.start_cluster;

    while(cluster < 0xFFF8 && remaining > 0){
        uint32_t sector = cluster_to_sector(cluster);
        for(int s=0;s<SECTORS_PER_CLUSTER && remaining>0;s++){
            if(disk_read_sector(sector+s, sector_buf)!=0) return -1;
            uint32_t to_copy = remaining < SECTOR_SIZE ? remaining : SECTOR_SIZE;
            f_memcpy(buf+pos, sector_buf, to_copy);
            pos += to_copy;
            remaining -= to_copy;
        }
        cluster = fat_get(cluster);
    }
    buf[pos]='\0';
    return (int)pos;
}

/* ===== Write file ===== */
int fat_write(const char* path, const char* buf, uint32_t len){
    if(!fs_ready) return -1;

    /* find or create dir entry */
    int idx = dir_find(path);
    DirEntry e;
    if(idx>=0){
        dir_read(idx,&e);
        /* free old clusters */
        if(e.start_cluster >= 2) fat_free_chain(e.start_cluster);
        e.start_cluster = 0xFFFF;
        e.size = 0;
    } else {
        idx = dir_alloc();
        if(idx<0) return -1;
        f_memset(&e, 0, sizeof(e));
        e.status = DIRENT_USED;
        f_strcpy(e.name, path);
        e.start_cluster = 0xFFFF;
        e.size = 0;
    }

    /* allocate clusters and write data */
    uint32_t remaining = len;
    uint32_t pos = 0;
    uint16_t first_cluster = 0xFFFF;
    uint16_t prev_cluster  = 0xFFFF;

    while(remaining > 0 || first_cluster == 0xFFFF){
        uint16_t cluster = fat_alloc();
        if(cluster == 0xFFFF) return -1; /* disk full */

        if(first_cluster == 0xFFFF) first_cluster = cluster;
        if(prev_cluster != 0xFFFF) fat_set(prev_cluster, cluster);
        fat_set(cluster, 0xFFFF); /* end of chain */
        prev_cluster = cluster;

        uint32_t sector = cluster_to_sector(cluster);
        for(int s=0;s<SECTORS_PER_CLUSTER;s++){
            f_memset(sector_buf, 0, SECTOR_SIZE);
            if(remaining > 0){
                uint32_t to_copy = remaining < SECTOR_SIZE ? remaining : SECTOR_SIZE;
                f_memcpy(sector_buf, buf+pos, to_copy);
                pos += to_copy;
                remaining = remaining > SECTOR_SIZE ? remaining - SECTOR_SIZE : 0;
            }
            if(disk_write_sector(sector+s, sector_buf)!=0) return -1;
        }
        if(remaining == 0) break;
    }

    e.start_cluster = first_cluster;
    e.size = len;
    dir_write(idx, &e);
    fat_flush();
    return (int)len;
}

/* ===== Delete ===== */
int fat_delete(const char* path){
    if(!fs_ready) return -1;
    int idx = dir_find(path);
    if(idx<0) return -1;
    DirEntry e; dir_read(idx,&e);
    fat_free_chain(e.start_cluster);
    e.status = DIRENT_DELETED;
    dir_write(idx,&e);
    fat_flush();
    return 0;
}

/* ===== List directory ===== */
int fat_list(const char* dir, FatEntry* entries, int max){
    if(!fs_ready) return 0;
    (void)dir; /* flat filesystem for now */
    int count=0;
    DirEntry e;
    for(int i=0;i<MAX_DIR_ENTRIES && count<max;i++){
        if(dir_read(i,&e)!=0) continue;
        if(e.status==DIRENT_USED || e.status==DIRENT_DIR){
            f_strcpy(entries[count].name, e.name);
            entries[count].size = e.size;
            entries[count].start_cluster = e.start_cluster;
            entries[count].is_dir = (e.status==DIRENT_DIR);
            count++;
        }
    }
    return count;
}

/* ===== Mkdir ===== */
int fat_mkdir(const char* path){
    if(!fs_ready) return -1;
    if(dir_find(path)>=0) return 0; /* already exists */
    int idx=dir_alloc();
    if(idx<0) return -1;
    DirEntry e;
    f_memset(&e,0,sizeof(e));
    e.status=DIRENT_DIR;
    f_strcpy(e.name,path);
    dir_write(idx,&e);
    return 0;
}

/* ===== Exists ===== */
int fat_exists(const char* path){
    if(!fs_ready) return 0;
    return dir_find(path)>=0;
}
