#include "usb_storage.h"
#include "usb_uhci.h"
#include "usb_enum.h"
#include "fat32_parser.h"
#include "io.h"
#include "screen.h"
#include "pit.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/stdlib.h"

static uint8_t bulk_in_ep = 0x81;  // Default Bulk IN endpoint
static uint8_t bulk_out_ep = 0x02; // Default Bulk OUT endpoint
static uint16_t bulk_max_packet = 64;

static uint8_t bulk_in_toggle = 0;
static uint8_t bulk_out_toggle = 0;
static uint32_t cbw_tag_counter = 1;
static uint16_t active_port_reg = 0; // Dynamic active port register

// Physical Memory Allocations for UHCI Bulk Descriptors (16-byte Aligned)
static uhci_td_t bulk_tds[16] __attribute__((aligned(16)));
static uhci_qh_t bulk_qh     __attribute__((aligned(16)));

static usb_cbw_t cbw __attribute__((aligned(16)));
static usb_csw_t csw __attribute__((aligned(16)));
static uint8_t config_buffer[256] __attribute__((aligned(16)));
static uint8_t sector_buffer[512] __attribute__((aligned(16)));

// Big-Endian to Host Conversion
static uint32_t be32_to_cpu(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Decode specific UHCI TD Status Error Bits for Debug Diagnostics
static const char* uhci_decode_td_error(uint32_t status) {
    if (status & (1 << 22)) return "STALL / Stalled Endpoint";
    if (status & (1 << 21)) return "Data Buffer Error";
    if (status & (1 << 20)) return "Babble Detected";
    if (status & (1 << 19)) return "NAK Received";
    if (status & (1 << 18)) return "CRC / Timeout Error";
    if (status & (1 << 17)) return "Bitstuff Error";
    if (status & (1 << 23)) return "Active Bit Still Set (Hardware Processing Stalled)";
    return "Unknown Status Error";
}

// USB BOT Reset Recovery with return value checking & critical error log
int usb_storage_reset_recovery(uint8_t dev_addr, uint16_t port_reg) {
    if (port_reg == 0) {
        port_reg = active_port_reg ? active_port_reg : (uhci_get_io_base() + UHCI_REG_PORTSC1);
    }

    printf("[USB Storage] Issuing USB Bulk-Only Mass Storage Reset (BOMSR) on Port 0x%X...\n", port_reg);

    // Clear USBSTS Write-1-to-Clear status bits
    outw(uhci_get_io_base() + UHCI_REG_USBSTS, 0x003F);

    int recovery_ok = 1;

    // 1. Bulk-Only Mass Storage Reset Control Request (0x21, 0xFF)
    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(usb_setup_packet_t));
    setup.bmRequestType = 0x21; // Host-to-Device, Class, Interface
    setup.bRequest = 0xFF;      // Bulk-Only Mass Storage Reset
    setup.wValue = 0x0000;
    setup.wIndex = 0x0000;      // Interface 0
    setup.wLength = 0x0000;
    recovery_ok &= uhci_exec_control_transfer(dev_addr, &setup, NULL, 0, 0, port_reg);
    pit_delay_ms(50);

    // 2. Clear Feature ENDPOINT_HALT on Bulk IN Endpoint
    memset(&setup, 0, sizeof(usb_setup_packet_t));
    setup.bmRequestType = 0x02; // Host-to-Device, Standard, Endpoint
    setup.bRequest = 0x01;      // CLEAR_FEATURE
    setup.wValue = 0x0000;      // ENDPOINT_HALT
    setup.wIndex = bulk_in_ep;
    setup.wLength = 0x0000;
    recovery_ok &= uhci_exec_control_transfer(dev_addr, &setup, NULL, 0, 0, port_reg);
    pit_delay_ms(50);

    // 3. Clear Feature ENDPOINT_HALT on Bulk OUT Endpoint
    setup.wIndex = bulk_out_ep;
    recovery_ok &= uhci_exec_control_transfer(dev_addr, &setup, NULL, 0, 0, port_reg);
    pit_delay_ms(50);

    // 4. Reset Data Toggles
    bulk_in_toggle = 0;
    bulk_out_toggle = 0;

    if (!recovery_ok) {
        printf("[USB Storage] CRITICAL: Device unresponsive - Reset Recovery FAILED (device may be disconnected or controller in bad state).\n");
        return 0;
    }

    printf("[USB Storage] BOT Reset Recovery Complete. Endpoints Cleared.\n");
    return 1;
}

// ============================================================================
// UHCI Bulk Transfer Execution Engine
// ============================================================================
static int uhci_exec_bulk_transfer(uint8_t dev_addr, uint8_t ep_addr, void *data_buf, uint16_t data_len, int is_in, uint8_t *toggle) {
    uint16_t io_base = uhci_get_io_base();
    if (io_base == 0) return 0;

    // Defensive Safeguard: Ensure UHCI Controller Run/Stop bit is set
    uint16_t usbcmd = inw(io_base + UHCI_REG_USBCMD);
    if ((usbcmd & UHCI_CMD_RS) == 0) {
        printf("[USB Recovery] Controller was stopped (USBCMD=0x%04X) — re-asserting Run/Stop bit before bulk transfer.\n", usbcmd);
        outw(io_base + UHCI_REG_USBCMD, UHCI_CMD_RS | UHCI_CMD_MAXP);
        pit_delay_ms(10);
    }

    // Clear USBSTS Write-1-to-Clear status bits BEFORE starting transfer
    outw(io_base + UHCI_REG_USBSTS, 0x003F);

    printf("[USB Bulk Debug] EP 0x%02X | Using Toggle=%u | Data Len=%u\n", ep_addr, *toggle, data_len);

    uint16_t sts_before = 0, cmd_before = 0;
    uhci_dump_registers(&sts_before, &cmd_before);

    memset(bulk_tds, 0, sizeof(bulk_tds));
    memset(&bulk_qh, 0, sizeof(uhci_qh_t));

    uint16_t bytes_remaining = data_len;
    uint8_t *buf_ptr = (uint8_t *)data_buf;
    int td_count = 0;

    while (bytes_remaining > 0 && td_count < 16) {
        uint16_t chunk = (bytes_remaining > bulk_max_packet) ? bulk_max_packet : bytes_remaining;

        bulk_tds[td_count].link_ptr = (bytes_remaining > chunk) ?
            (((uint32_t)&bulk_tds[td_count + 1]) | UHCI_PTR_DEPTH_FIRST) : UHCI_PTR_TERMINATE;
            
        bulk_tds[td_count].status = (1 << 23) | (3 << 27); // Active = 1, C_ERR = 3
        
        uint8_t pid = is_in ? USB_PID_IN : USB_PID_OUT;
        uint8_t ep_num = ep_addr & 0x0F;
        uint32_t dt = (*toggle) ? (1 << 19) : 0;
        
        bulk_tds[td_count].header = (pid) | ((uint32_t)dev_addr << 8) | ((uint32_t)ep_num << 15) | dt | (((uint32_t)(chunk - 1)) << 21);
        bulk_tds[td_count].buffer_ptr = (uint32_t)buf_ptr;

        buf_ptr += chunk;
        bytes_remaining -= chunk;
        *toggle ^= 1; // Toggle DATA0 / DATA1 for next packet
        td_count++;
    }

    bulk_qh.head_ptr = UHCI_PTR_TERMINATE;
    bulk_qh.element_ptr = (uint32_t)&bulk_tds[0];

    // Link Bulk QH to Frame List
    uint32_t *frame_list = uhci_get_frame_list();
    if (frame_list) {
        uint32_t qh_ptr = ((uint32_t)&bulk_qh) | UHCI_PTR_QH;
        for (int i = 0; i < 1024; i++) {
            frame_list[i] = qh_ptr;
        }
    }

    // Hardware Completion Poll Loop
    int last_td = td_count - 1;
    int timeout = 100000;
    while ((bulk_tds[last_td].status & (1 << 23)) && --timeout > 0) {
        int error_found = 0;
        for (int i = 0; i < td_count; i++) {
            uint32_t st = bulk_tds[i].status;
            if (st & 0x007E0000) { // Error bits 17..22 set
                error_found = 1;
                break;
            }
        }
        if (error_found) break;

        pit_delay_ms(1);
    }

    // Unlink Bulk QH from Frame List
    if (frame_list) {
        for (int i = 0; i < 1024; i++) {
            frame_list[i] = UHCI_PTR_TERMINATE;
        }
    }

    uint16_t sts_after = 0, cmd_after = 0;
    uhci_dump_registers(&sts_after, &cmd_after);

    printf("[USB Debug] USBSTS Before=0x%04X After=0x%04X (HCHalted=%d, Error=%d) | USBCMD=0x%04X\n",
           sts_before, sts_after, (sts_after & UHCI_STS_HCHALTED) ? 1 : 0, (sts_after & UHCI_STS_ERROR) ? 1 : 0, cmd_after);

    if (sts_after & UHCI_STS_HCHALTED) {
        uhci_recover_if_halted();
    }

    // Scan ALL TDs (0 to last_td) to locate FIRST failing TD
    int first_failed_td = -1;
    for (int i = 0; i < td_count; i++) {
        uint32_t st = bulk_tds[i].status;
        if ((st & (1 << 23)) || (st & 0x007E0000)) { // Active or Error bit set
            first_failed_td = i;
            break;
        }
    }

    if (first_failed_td >= 0) {
        uint32_t failed_status = bulk_tds[first_failed_td].status;
        printf("[USB Bulk Error] TD #%d of %d failed (Endpoint 0x%02X, Status: 0x%08X) - [Reason: %s]\n",
               first_failed_td, td_count, ep_addr, failed_status, uhci_decode_td_error(failed_status));
        return 0;
    }

    return 1; // Success!
}

// ============================================================================
// SCSI Command Execution Wrapper
// ============================================================================
static int usb_storage_send_scsi_cmd_internal(uint8_t dev_addr, const uint8_t *cdb, uint8_t cdb_len, void *data_buf, uint32_t data_len, int is_in) {
    memset(&cbw, 0, sizeof(usb_cbw_t));
    memset(&csw, 0, sizeof(usb_csw_t));

    uint32_t tag = cbw_tag_counter++;

    // Construct Command Block Wrapper (CBW - 31 Bytes)
    cbw.dCBWSignature = USB_CBW_SIGNATURE; // "USBC"
    cbw.dCBWTag = tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags = is_in ? 0x80 : 0x00;
    cbw.bCBWLUN = 0;
    cbw.bCBWCBLength = cdb_len;
    memcpy(cbw.CBWCB, cdb, cdb_len);

    // USB BOT Spec Rule 1: CBW MUST ALWAYS be sent with DATA0 toggle (Toggle = 0)!
    bulk_out_toggle = 0;

    // Step A: Send CBW over Bulk OUT Endpoint
    if (!uhci_exec_bulk_transfer(dev_addr, bulk_out_ep, &cbw, sizeof(usb_cbw_t), 0, &bulk_out_toggle)) {
        printf("[USB Storage] Error: Failed to send CBW command packet.\n");
        return 0;
    }

    // USB BOT Spec Rule 2: Data Stage MUST start with DATA1 toggle (Toggle = 1)!
    if (data_len > 0 && data_buf != NULL) {
        if (is_in) {
            bulk_in_toggle = 1; // Data IN stage starts with DATA1
            if (!uhci_exec_bulk_transfer(dev_addr, bulk_in_ep, data_buf, (uint16_t)data_len, 1, &bulk_in_toggle)) {
                printf("[USB Storage] Error: Failed during Data IN Stage.\n");
                return 0;
            }
        } else {
            bulk_out_toggle = 1; // Data OUT stage starts with DATA1
            if (!uhci_exec_bulk_transfer(dev_addr, bulk_out_ep, data_buf, (uint16_t)data_len, 0, &bulk_out_toggle)) {
                printf("[USB Storage] Error: Failed during Data OUT Stage.\n");
                return 0;
            }
        }
    }

    // USB BOT Spec Rule 3: CSW Stage MUST ALWAYS be received with DATA1 toggle (Toggle = 1)!
    bulk_in_toggle = 1;

    // Step C: Read CSW (13 Bytes) over Bulk IN Endpoint
    if (!uhci_exec_bulk_transfer(dev_addr, bulk_in_ep, &csw, sizeof(usb_csw_t), 1, &bulk_in_toggle)) {
        printf("[USB Storage] Error: Failed to receive CSW status packet.\n");
        return 0;
    }

    // Validate CSW
    if (csw.dCSWSignature != USB_CSW_SIGNATURE) {
        printf("[USB Storage] CSW Validation Error: Invalid Signature 0x%08X (Expected 0x53425355).\n", csw.dCSWSignature);
        return 0;
    }

    if (csw.dCSWTag != tag) {
        printf("[USB Storage] CSW Validation Error: Tag mismatch (Got 0x%X, Expected 0x%X).\n", csw.dCSWTag, tag);
        return 0;
    }

    if (csw.bCSWStatus != 0) {
        printf("[USB Storage] CSW Status Error: Command failed with status 0x%02X.\n", csw.bCSWStatus);
        return 0;
    }

    return 1; // Success!
}

static int usb_storage_send_scsi_cmd(uint8_t dev_addr, const uint8_t *cdb, uint8_t cdb_len, void *data_buf, uint32_t data_len, int is_in) {
    uint16_t port_reg = active_port_reg ? active_port_reg : (uhci_get_io_base() + UHCI_REG_PORTSC1);

    // Pre-Command Port Status & Connection Check (CCS Bit)
    uint16_t portsc = inw(port_reg);
    if ((portsc & UHCI_PORTSC_CCS) == 0) {
        printf("[USB Storage] Error: Device disconnected on Port 0x%X (CCS=0, Status=0x%04X).\n", port_reg, portsc);
        return 0;
    } else {
        printf("[USB Storage] Device Connection Verified on Port 0x%X (Status: 0x%04X, CCS=1)\n", port_reg, portsc);
    }

    for (int retry = 0; retry < 3; retry++) {
        if (retry > 0) {
            printf("[USB Storage] SCSI Command Retry #%d (Port 0x%X)...\n", retry, port_reg);
            pit_delay_ms(50);

            // Abort retry loop immediately if Reset Recovery itself fails
            if (!usb_storage_reset_recovery(dev_addr, port_reg)) {
                printf("[USB Storage] Reset Recovery failed on retry #%d -> Aborting command execution.\n", retry);
                break;
            }
        }

        if (usb_storage_send_scsi_cmd_internal(dev_addr, cdb, cdb_len, data_buf, data_len, is_in)) {
            return 1; // Success!
        }
    }

    printf("[USB Storage] Error: SCSI Command failed.\n");
    return 0;
}

// ============================================================================
// USB Storage Initialization & SCSI Command Calls
// ============================================================================
int usb_storage_init(uint8_t dev_addr, uint16_t port_reg) {
    active_port_reg = port_reg; // Store active port register dynamically
    printf("[USB Storage] Fetching Configuration Descriptor (64 Bytes) on Port 0x%X...\n", port_reg);

    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(usb_setup_packet_t));
    setup.bmRequestType = 0x80;        // Device-to-Host
    setup.bRequest = 0x06;             // GET_DESCRIPTOR
    setup.wValue = (USB_DESC_TYPE_CONFIG << 8) | 0x00; // CONFIGURATION Descriptor 0
    setup.wIndex = 0x0000;
    setup.wLength = 64;

    memset(config_buffer, 0, sizeof(config_buffer));

    if (!uhci_exec_control_transfer(dev_addr, &setup, config_buffer, 64, 1, port_reg)) {
        printf("[USB Storage] Warning: Control transfer failed on Port 0x%X, attempting fallback endpoint parsing...\n", port_reg);
    }

    // Parse Configuration, Interface, & Endpoint Descriptors
    uint8_t *ptr = config_buffer;
    uint8_t *end = config_buffer + 64;
    uint8_t config_val = 1;

    while (ptr < end && ptr[0] > 0) {
        uint8_t len = ptr[0];
        uint8_t type = ptr[1];

        if (type == USB_DESC_TYPE_CONFIG) {
            usb_config_descriptor_t *cfg = (usb_config_descriptor_t *)ptr;
            config_val = cfg->bConfigurationValue;
        } else if (type == USB_DESC_TYPE_ENDPOINT) {
            usb_endpoint_descriptor_t *ep = (usb_endpoint_descriptor_t *)ptr;
            if ((ep->bmAttributes & 0x03) == 0x02) { // Bulk Endpoint
                if (ep->bEndpointAddress & 0x80) {
                    bulk_in_ep = ep->bEndpointAddress;
                } else {
                    bulk_out_ep = ep->bEndpointAddress;
                }
                if (ep->wMaxPacketSize > 0) {
                    bulk_max_packet = ep->wMaxPacketSize;
                }
            }
        }
        ptr += len;
    }

    printf("[USB Storage] Bulk Endpoints Detected -> Bulk IN: 0x%02X, Bulk OUT: 0x%02X (Max Packet: %d bytes)\n",
           bulk_in_ep, bulk_out_ep, bulk_max_packet);

    // SET_CONFIGURATION
    printf("[USB Storage] Sending SET_CONFIGURATION (%d) to Address %d...\n", config_val, dev_addr);
    memset(&setup, 0, sizeof(usb_setup_packet_t));
    setup.bmRequestType = 0x00;        // Host-to-Device
    setup.bRequest = 0x09;             // SET_CONFIGURATION
    setup.wValue = config_val;
    setup.wIndex = 0x0000;
    setup.wLength = 0;

    uhci_exec_control_transfer(dev_addr, &setup, NULL, 0, 0, port_reg);
    pit_delay_ms(10);

    bulk_in_toggle = 0;
    bulk_out_toggle = 0;

    printf("[USB Storage] Device configured successfully. Ready for SCSI BOT transfers.\n");

    // SCSI INQUIRY
    usb_storage_inquiry(dev_addr);

    // SCSI READ CAPACITY (10)
    uint32_t total_blocks = 0;
    uint32_t block_size = 512;
    usb_storage_read_capacity(dev_addr, &total_blocks, &block_size);

    // SCSI READ (10) Sector 0 Hex Dump
    printf("[USB Storage] Reading LBA Sector 0 (Boot Sector / MBR)...\n");
    memset(sector_buffer, 0, sizeof(sector_buffer));
    if (usb_storage_read_sector(dev_addr, 0, sector_buffer)) {
        printf("[USB Storage] LBA Sector 0 Read Success (512 Bytes):\n");
        usb_storage_hex_dump(sector_buffer, 128);

        // Trigger Layer 4: Real FAT32 BPB Parser Mount!
        fat32_mount();
    }

    return 1;
}

// ============================================================================
// SCSI INQUIRY Command (0x12)
// ============================================================================
int usb_storage_inquiry(uint8_t dev_addr) {
    printf("[USB Storage] Executing SCSI INQUIRY (0x12)...\n");

    uint8_t cdb[6] = {SCSI_CMD_INQUIRY, 0x00, 0x00, 0x00, 36, 0x00};
    uint8_t inq_buf[36];
    memset(inq_buf, 0, sizeof(inq_buf));

    if (!usb_storage_send_scsi_cmd(dev_addr, cdb, 6, inq_buf, 36, 1)) {
        printf("[USB Storage] SCSI INQUIRY Failed.\n");
        return 0;
    }

    char vendor[9];
    char product[17];
    char revision[5];

    memcpy(vendor, &inq_buf[8], 8);     vendor[8] = '\0';
    memcpy(product, &inq_buf[16], 16);  product[16] = '\0';
    memcpy(revision, &inq_buf[32], 4);  revision[4] = '\0';

    printf("====================================================\n");
    printf("           SCSI INQUIRY Device Details              \n");
    printf("====================================================\n");
    printf("  Vendor ID        : %s\n", vendor);
    printf("  Product ID       : %s\n", product);
    printf("  Product Revision : %s\n", revision);
    printf("====================================================\n");

    return 1;
}

// ============================================================================
// SCSI READ CAPACITY (10) Command (0x25)
// ============================================================================
int usb_storage_read_capacity(uint8_t dev_addr, uint32_t *out_block_count, uint32_t *out_block_size) {
    printf("[USB Storage] Executing SCSI READ CAPACITY (10) (0x25)...\n");

    uint8_t cdb[10] = {SCSI_CMD_READ_CAPACITY_10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t cap_buf[8];
    memset(cap_buf, 0, sizeof(cap_buf));

    if (!usb_storage_send_scsi_cmd(dev_addr, cdb, 10, cap_buf, 8, 1)) {
        printf("[USB Storage] SCSI READ CAPACITY Failed.\n");
        return 0;
    }

    uint32_t max_lba = be32_to_cpu(&cap_buf[0]);
    uint32_t block_len = be32_to_cpu(&cap_buf[4]);

    uint32_t total_blocks = max_lba + 1;
    uint32_t size_mb = (uint32_t)(((uint64_t)total_blocks * block_len) / (1024 * 1024));

    printf("====================================================\n");
    printf("         USB Drive Capacity Information             \n");
    printf("====================================================\n");
    printf("  Total Block Count : %u sectors\n", total_blocks);
    printf("  Sector / Block Size: %u bytes\n", block_len);
    printf("  Total Drive Size  : %u MB\n", size_mb);
    printf("====================================================\n");

    if (out_block_count) *out_block_count = total_blocks;
    if (out_block_size)  *out_block_size = block_len;

    return 1;
}

// ============================================================================
// SCSI READ (10) Command (0x28) - Read Physical LBA Sector
// ============================================================================
int usb_storage_read_sector(uint8_t dev_addr, uint32_t lba, uint8_t *buffer) {
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_CMD_READ_10;
    cdb[2] = (uint8_t)((lba >> 24) & 0xFF);
    cdb[3] = (uint8_t)((lba >> 16) & 0xFF);
    cdb[4] = (uint8_t)((lba >> 8) & 0xFF);
    cdb[5] = (uint8_t)(lba & 0xFF);
    cdb[7] = 0x00;
    cdb[8] = 0x01; // 1 sector (512 bytes)

    return usb_storage_send_scsi_cmd(dev_addr, cdb, 10, buffer, 512, 1);
}

// ============================================================================
// SCSI WRITE (10) Command (0x2A) - Write Physical LBA Sector to USB Drive
// ============================================================================
int usb_storage_write_sector(uint8_t dev_addr, uint32_t lba, const uint8_t *buffer) {
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_CMD_WRITE_10; // 0x2A
    cdb[2] = (uint8_t)((lba >> 24) & 0xFF);
    cdb[3] = (uint8_t)((lba >> 16) & 0xFF);
    cdb[4] = (uint8_t)((lba >> 8) & 0xFF);
    cdb[5] = (uint8_t)(lba & 0xFF);
    cdb[7] = 0x00;
    cdb[8] = 0x01; // 1 sector (512 bytes)

    return usb_storage_send_scsi_cmd(dev_addr, cdb, 10, (void *)buffer, 512, 0); // is_in = 0 (Data OUT)
}

// Format 512-byte Hex Dump for Terminal Output
void usb_storage_hex_dump(const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; i += 16) {
        printf("  0x%04X: ", (unsigned int)i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < size) {
                printf("%02X ", data[i + j]);
            } else {
                printf("   ");
            }
        }
        printf(" |");
        for (size_t j = 0; j < 16; j++) {
            if (i + j < size) {
                uint8_t c = data[i + j];
                if (c >= 32 && c <= 126) {
                    printf("%c", c);
                } else {
                    printf(".");
                }
            }
        }
        printf("|\n");
    }
}
