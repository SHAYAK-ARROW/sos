#ifndef USB_STORAGE_H
#define USB_STORAGE_H

#include <stdint.h>
#include <stddef.h>
#include "usb_enum.h"
#include "pci.h"

// ============================================================================
// USB Descriptors
// ============================================================================
#define USB_DESC_TYPE_CONFIG    0x02
#define USB_DESC_TYPE_INTERFACE 0x04
#define USB_DESC_TYPE_ENDPOINT  0x05

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} __attribute__((packed)) usb_config_descriptor_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed)) usb_interface_descriptor_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed)) usb_endpoint_descriptor_t;

// ============================================================================
// USB Mass Storage Bulk-Only Transport (BOT) Structures
// ============================================================================
#define USB_CBW_SIGNATURE 0x43425355 // "USBC"
#define USB_CSW_SIGNATURE 0x53425355 // "USBS"

typedef struct {
    uint32_t dCBWSignature;          // 0x43425355 ("USBC")
    uint32_t dCBWTag;                // Transaction Tag
    uint32_t dCBWDataTransferLength; // Transfer Length
    uint8_t bmCBWFlags;              // Direction (0x80 = IN, 0x00 = OUT)
    uint8_t bCBWLUN;                 // Logical Unit Number (0)
    uint8_t bCBWCBLength;            // SCSI Command Length
    uint8_t CBWCB[16];               // SCSI Command Descriptor Block
} __attribute__((packed)) usb_cbw_t; // Exact 31 Bytes

typedef struct {
    uint32_t dCSWSignature;          // 0x53425355 ("USBS")
    uint32_t dCSWTag;                // Matches dCBWTag
    uint32_t dCSWDataResidue;        // Data Residue
    uint8_t bCSWStatus;              // 0 = Passed, 1 = Failed, 2 = Phase Error
} __attribute__((packed)) usb_csw_t; // Exact 13 Bytes

// ============================================================================
// SCSI Command Opcodes
// ============================================================================
#define SCSI_CMD_INQUIRY           0x12
#define SCSI_CMD_READ_CAPACITY_10  0x25
#define SCSI_CMD_READ_10           0x28
#define SCSI_CMD_WRITE_10          0x2A

// Function Prototypes
int usb_storage_init(uint8_t dev_addr, uint16_t port_reg);
int usb_storage_reset_recovery(uint8_t dev_addr, uint16_t port_reg);
int usb_storage_inquiry(uint8_t dev_addr);
int usb_storage_read_capacity(uint8_t dev_addr, uint32_t *out_block_count, uint32_t *out_block_size);
int usb_storage_read_sector(uint8_t dev_addr, uint32_t lba, uint8_t *buffer);
int usb_storage_write_sector(uint8_t dev_addr, uint32_t lba, const uint8_t *buffer);
void usb_storage_hex_dump(const uint8_t *data, size_t size);

#endif
