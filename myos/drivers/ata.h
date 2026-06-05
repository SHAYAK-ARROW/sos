#ifndef ATA_H
#define ATA_H
#include <stdint.h>

int  ata_init(void);
int  ata_read_sector(uint32_t lba, uint8_t* buf);
int  ata_write_sector(uint32_t lba, uint8_t* buf);

#endif
