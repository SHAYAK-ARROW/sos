#ifndef DISK_H
#define DISK_H

#include <stdint.h>

/*
 * disk.h — unified disk interface
 *
 * fat16.c etar poriborte ata_read_sector/ata_write_sector er bodole
 * disk_read_sector/disk_write_sector call korbe.
 *
 * Automatically USB pen drive use kore, na paile ATA/HDD te fallback kore.
 */

/* Konta disk connected ache seta print kore — kernel_main theke call korbe */
void disk_init(void);

/* fat16.c theke ei duto function call kora hobe */
int disk_read_sector (uint32_t lba, uint8_t* buf);
int disk_write_sector(uint32_t lba, uint8_t* buf);

/* 1 = kono disk ready, 0 = kono disk nai */
int disk_present(void);

#endif
