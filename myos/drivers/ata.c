#include "ata.h"
#include <stdint.h>

/* ATA PIO ports - Primary bus */
#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECTOR_CNT  0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7

#define ATA_STATUS_BSY  0x80
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_ERR  0x01

#define ATA_CMD_READ    0x20
#define ATA_CMD_WRITE   0x30
#define ATA_CMD_IDENTIFY 0xEC

static inline void outb(uint16_t port, uint8_t val){
    __asm__ volatile("outb %0,%1"::"a"(val),"Nd"(port));
}
static inline uint8_t inb(uint16_t port){
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v;
}
static inline uint16_t inw(uint16_t port){
    uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(port)); return v;
}
static inline void outw(uint16_t port, uint16_t val){
    __asm__ volatile("outw %0,%1"::"a"(val),"Nd"(port));
}

static void ata_wait(void){
    /* wait until BSY clears */
    int timeout = 100000;
    while((inb(ATA_STATUS) & ATA_STATUS_BSY) && timeout > 0) timeout--;
}

static void ata_wait_drq(void){
    int timeout = 100000;
    /* Fixed: Now properly breaks on ATA_STATUS_ERR */
    while(!(inb(ATA_STATUS) & (ATA_STATUS_DRQ | ATA_STATUS_ERR)) && timeout > 0) timeout--;
}

static int disk_present = 0;

int ata_init(void){
    /* select master drive */
    outb(ATA_DRIVE_HEAD, 0xA0);
    ata_wait();
    /* send IDENTIFY */
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_wait();
    uint8_t status = inb(ATA_STATUS);
    if(status == 0){ disk_present = 0; return -1; }
    if(status & ATA_STATUS_ERR){ disk_present = 0; return -1; }
    /* read identify data */
    for(int i=0;i<256;i++) inw(ATA_DATA);
    disk_present = 1;
    return 0;
}

int ata_read_sector(uint32_t lba, uint8_t* buf){
    if(!disk_present) return -1;
    ata_wait();
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECTOR_CNT, 1);
    outb(ATA_LBA_LO,  (uint8_t)(lba));
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI,  (uint8_t)(lba >> 16));
    outb(ATA_COMMAND, ATA_CMD_READ);
    ata_wait();
    ata_wait_drq();
    if(inb(ATA_STATUS) & ATA_STATUS_ERR) return -1;
    uint16_t* ptr = (uint16_t*)buf;
    for(int i=0;i<256;i++) ptr[i] = inw(ATA_DATA);
    return 0;
}

int ata_write_sector(uint32_t lba, uint8_t* buf){
    if(!disk_present) return -1;
    ata_wait();
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECTOR_CNT, 1);
    outb(ATA_LBA_LO,  (uint8_t)(lba));
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI,  (uint8_t)(lba >> 16));
    outb(ATA_COMMAND, ATA_CMD_WRITE);
    ata_wait();
    ata_wait_drq();
    if(inb(ATA_STATUS) & ATA_STATUS_ERR) return -1;
    uint16_t* ptr = (uint16_t*)buf;
    for(int i=0;i<256;i++) outw(ATA_DATA, ptr[i]);
    /* flush cache */
    outb(ATA_COMMAND, 0xE7);
    ata_wait();
    return 0;
}
