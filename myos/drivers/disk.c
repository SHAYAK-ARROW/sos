/*
 * disk.c — unified disk abstraction
 *
 * Priority:
 *   1. USB pen drive  (usb_init success hole)
 *   2. ATA/IDE HDD    (ata_init success hole)
 *   3. Kono disk nai  (RAM-only mode)
 */

#include "disk.h"
#include "usb.h"
#include "ata.h"
#include <stdint.h>

extern void terminal_setcolor(uint8_t fg, uint8_t bg);
extern void terminal_write(const char* s);
extern void terminal_writeline(const char* s);

/* simple number print helper */
static void print_num(uint32_t n){
    char buf[12];
    int i = 10;
    buf[11] = '\0';
    if(n == 0){ terminal_write("0"); return; }
    while(n && i >= 0){ buf[i--] = '0' + (n % 10); n /= 10; }
    terminal_write(buf + i + 1);
}

#define DISK_NONE 0
#define DISK_USB  1
#define DISK_ATA  2

static int g_disk_type = DISK_NONE;

void disk_init(void){
    /* Prothome USB try koro */
    terminal_write("USB: scanning... ");
    if(usb_init() == 0){
        g_disk_type = DISK_USB;
        terminal_setcolor(10, 0);
        terminal_write("Pen drive found! ");

        uint32_t p2lba  = usb_part2_lba();
        uint32_t p2size = usb_part2_size();

        if(p2lba == 0 && p2size == 0){
            terminal_writeline("No partition table — using full disk (raw mode).");
        } else {
            terminal_write("Partition 2: LBA=");
            print_num(p2lba);
            terminal_write(", size=");
            print_num(p2size / 2);   /* sectors -> KB */
            terminal_writeline(" KB — using as storage.");
        }
        terminal_setcolor(7, 0);
        return;
    }
    terminal_setcolor(12, 0);
    terminal_writeline("not found.");
    terminal_setcolor(7, 0);

    /* Tarpor ATA try koro */
    terminal_write("ATA: scanning... ");
    if(ata_init() == 0){
        g_disk_type = DISK_ATA;
        terminal_setcolor(10, 0);
        terminal_writeline("HDD found! Using ATA disk.");
        terminal_setcolor(7, 0);
        return;
    }
    terminal_setcolor(12, 0);
    terminal_writeline("not found. RAM-only mode.");
    terminal_setcolor(7, 0);

    g_disk_type = DISK_NONE;
}

int disk_read_sector(uint32_t lba, uint8_t* buf){
    if(g_disk_type == DISK_USB) return usb_read_sector(lba, buf);
    if(g_disk_type == DISK_ATA) return ata_read_sector(lba, buf);
    return -1;
}

int disk_write_sector(uint32_t lba, uint8_t* buf){
    if(g_disk_type == DISK_USB) return usb_write_sector(lba, buf);
    if(g_disk_type == DISK_ATA) return ata_write_sector(lba, buf);
    return -1;
}

int disk_present(void){
    return g_disk_type != DISK_NONE;
}
