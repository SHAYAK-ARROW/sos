#ifndef USB_H
#define USB_H

#include <stdint.h>

// USB Device Descriptor Types
#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIG         0x02
#define USB_DESC_STRING         0x03
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05

// USB Class Codes
#define USB_CLASS_HID           0x03
#define USB_CLASS_MASS_STORAGE  0x08

// USB Mass Storage Subclasses
#define USB_SUBCLASS_SCSI       0x06
#define USB_PROTOCOL_BOT        0x50 // Bulk-Only Transport

typedef struct {
    uint8_t address;
    uint8_t speed;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t class_code;
    uint8_t subclass_code;
    uint8_t protocol_code;
    uint8_t is_connected;
} usb_device_t;

// USB Setup Packet Header
typedef struct {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_packet_t;

// USB Mass Storage Command Block Wrapper (CBW)
typedef struct {
    uint32_t dCBWSignature;   // 0x43425355 ("USBC")
    uint32_t dCBWTag;         // Unique transaction tag
    uint32_t dCBWDataTransferLength;
    uint8_t bmCBWFlags;       // Bit 7: Direction (0=Out, 1=In)
    uint8_t bCBWLUN;          // Logical Unit Number
    uint8_t bCBWCBLength;     // Length of SCSI command (1..16)
    uint8_t CBWCB[16];        // SCSI Command descriptor block
} __attribute__((packed)) usb_msd_cbw_t;

// USB Mass Storage Command Status Wrapper (CSW)
typedef struct {
    uint32_t dCSWSignature;   // 0x53425355 ("USBS")
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t bCSWStatus;       // 0=Success, 1=Failed, 2=Phase Error
} __attribute__((packed)) usb_msd_csw_t;

void usb_init(void);
void usb_poll(void);
int usb_read_sector(uint32_t lba, uint8_t *buffer);
int usb_write_sector(uint32_t lba, const uint8_t *buffer);
char usb_hid_decode_scancode(uint8_t keycode);

#endif
