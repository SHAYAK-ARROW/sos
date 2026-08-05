#ifndef USB_ENUM_H
#define USB_ENUM_H

#include <stdint.h>
#include "pci.h"

// ============================================================================
// UHCI Transfer Descriptor (TD) - 16 Bytes (volatile for DMA memory updates)
// ============================================================================
typedef struct {
    volatile uint32_t link_ptr;   // Link Pointer (Bit 0: T, Bit 1: QH/TD, Bit 2: VF)
    volatile uint32_t status;     // Status DWORD (Bit 23: Active, Bit 28-27: C_ERR, Error flags)
    volatile uint32_t header;     // Header DWORD (PID, Dev Addr, Endp, Data Toggle, Max Length)
    volatile uint32_t buffer_ptr; // Physical RAM Address of Data Payload Buffer
} __attribute__((packed, aligned(16))) uhci_td_t;

// ============================================================================
// UHCI Queue Head (QH) - 8 Bytes (volatile for DMA memory updates)
// ============================================================================
typedef struct {
    volatile uint32_t head_ptr;    // Horizontal Link Pointer
    volatile uint32_t element_ptr; // Vertical Link Pointer (First TD in Queue)
} __attribute__((packed, aligned(16))) uhci_qh_t;

// ============================================================================
// USB Packet ID (PID) Definitions
// ============================================================================
#define USB_PID_SETUP  0x2D // Host-to-Device Setup Packet
#define USB_PID_IN     0x69 // Device-to-Host Data Packet
#define USB_PID_OUT    0xE1 // Host-to-Device Data Packet

// ============================================================================
// USB Setup Packet - 8 Bytes (Standard USB 1.1 Specification)
// ============================================================================
typedef struct {
    uint8_t bmRequestType; // Bit 7: Direction (0=Out, 1=In), Type & Recipient
    uint8_t bRequest;      // Standard Request Code (0x06=GET_DESCRIPTOR, 0x05=SET_ADDRESS)
    uint16_t wValue;       // Parameter Value
    uint16_t wIndex;       // Index / Language ID
    uint16_t wLength;      // Number of bytes to transfer
} __attribute__((packed)) usb_setup_packet_t;

// ============================================================================
// Standard USB Device Descriptor - 18 Bytes
// ============================================================================
typedef struct {
    uint8_t bLength;            // Size of Descriptor (18 bytes)
    uint8_t bDescriptorType;     // DEVICE Descriptor Type (0x01)
    uint16_t bcdUSB;            // USB Spec Release (e.g. 0x0110 for USB 1.1)
    uint8_t bDeviceClass;       // Class Code (0x08 = Mass Storage, 0x03 = HID)
    uint8_t bDeviceSubClass;    // Subclass Code
    uint8_t bDeviceProtocol;    // Protocol Code
    uint8_t bMaxPacketSize0;    // Max Packet Size for Endpoint 0 (8, 16, 32, 64)
    uint16_t idVendor;          // Vendor ID (e.g. 0x0781 SanDisk)
    uint16_t idProduct;         // Product ID
    uint16_t bcdDevice;         // Device Release Number
    uint8_t iManufacturer;      // Index of Manufacturer String
    uint8_t iProduct;           // Index of Product String
    uint8_t iSerialNumber;      // Index of Serial Number String
    uint8_t bNumConfigurations; // Number of Possible Configurations
} __attribute__((packed)) usb_device_descriptor_t;

// Function Prototypes
int usb_reset_port(uint16_t port_reg);
int usb_enumerate_device(uint16_t port_num);
void usb_print_device_info(const usb_device_descriptor_t *desc);
int usb_is_port_enumerated(int port_num);
void usb_set_port_enumerated(int port_num, int state);
int uhci_exec_control_transfer(uint8_t dev_addr, usb_setup_packet_t *setup, void *data_buf, uint16_t data_len, int is_in, uint16_t port_reg);

#endif
