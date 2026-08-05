#include "usb_enum.h"
#include "usb_uhci.h"
#include "usb_storage.h"
#include "io.h"
#include "screen.h"
#include "pit.h"
#include "libc/stdio.h"
#include "libc/string.h"

// Track enumeration state per port (Port 1 and Port 2)
static int port_enumerated[2] = {0, 0};

// Physical Memory Allocations for UHCI Structures (16-byte Aligned)
static uhci_td_t setup_td __attribute__((aligned(16)));
static uhci_td_t data_td  __attribute__((aligned(16)));
static uhci_td_t status_td __attribute__((aligned(16)));
static uhci_qh_t control_qh __attribute__((aligned(16)));

static usb_setup_packet_t setup_pkt __attribute__((aligned(16)));
static uint8_t descriptor_buffer[64] __attribute__((aligned(16)));

int usb_is_port_enumerated(int port_num) {
    if (port_num >= 1 && port_num <= 2) {
        return port_enumerated[port_num - 1];
    }
    return 0;
}

void usb_set_port_enumerated(int port_num, int state) {
    if (port_num >= 1 && port_num <= 2) {
        port_enumerated[port_num - 1] = state;
    }
}

// ============================================================================
// 1. Port Reset (Calibrated PIT Delay)
// ============================================================================
int usb_reset_port(uint16_t port_reg) {
    printf("[USB Enum] Resetting Root Hub Port at I/O 0x%X...\n", port_reg);

    uint16_t portsc = inw(port_reg);
    if ((portsc & UHCI_PORTSC_CCS) == 0) return 0;

    // PR (Port Reset - Bit 9) 50ms hold using PIT Hardware Timer
    outw(port_reg, portsc | UHCI_PORTSC_PR);
    pit_delay_ms(50);

    // Clear PR bit
    outw(port_reg, inw(port_reg) & ~UHCI_PORTSC_PR);
    pit_delay_ms(10);

    // Set PE (Port Enable - Bit 2)
    outw(port_reg, inw(port_reg) | UHCI_PORTSC_PE);
    pit_delay_ms(20);

    portsc = inw(port_reg);
    if (portsc & UHCI_PORTSC_PE) {
        printf("[USB Enum] Port 0x%X Enabled Successfully (Status: 0x%X).\n", port_reg, portsc);
        return 1;
    }

    printf("[USB Enum] Warning: Port enable bit not set, retrying port enable...\n");
    outw(port_reg, inw(port_reg) | UHCI_PORTSC_PE);
    pit_delay_ms(20);

    portsc = inw(port_reg);
    if (portsc & UHCI_PORTSC_PE) {
        printf("[USB Enum] Port 0x%X Enabled Successfully on Retry (Status: 0x%X).\n", port_reg, portsc);
        return 1;
    }

    printf("[USB Enum] Error: Port 0x%X failed to enable after retry (Status: 0x%X).\n", port_reg, portsc);
    return 0;
}

// ============================================================================
// 2. UHCI Control Transfer Execution (Hardware USBSTS Poll + USBCMD Run/Stop Safeguard)
// ============================================================================
int uhci_exec_control_transfer(uint8_t dev_addr, usb_setup_packet_t *setup, void *data_buf, uint16_t data_len, int is_in, uint16_t port_reg) {
    uint16_t io_base = uhci_get_io_base();
    if (io_base == 0) return 0;

    // Proactive Safeguard: Ensure UHCI Controller is in RUNNING state
    uhci_ensure_running();

    memset(&setup_td, 0, sizeof(uhci_td_t));
    memset(&data_td, 0, sizeof(uhci_td_t));
    memset(&status_td, 0, sizeof(uhci_td_t));
    memset(&control_qh, 0, sizeof(uhci_qh_t));

    // Clear USBSTS register status flags before transfer
    outw(io_base + UHCI_REG_USBSTS, 0x00FF);

    // Check if Low-Speed device attached (Bit 8 LSDA in PORTSC)
    uint16_t portsc = inw(port_reg);
    uint32_t ls_bit = (portsc & UHCI_PORTSC_LSDA) ? (1 << 26) : 0; // Bit 26 LS in TD Status

    // Stage 1: SETUP Stage TD (PID 0x2D, DATA0 Toggle)
    setup_td.link_ptr = ((data_len > 0) ? ((uint32_t)&data_td) : ((uint32_t)&status_td)) | UHCI_PTR_DEPTH_FIRST;
    setup_td.status = (1 << 23) | (3 << 27) | ls_bit; // Active = 1 (bit 23), C_ERR = 3 (bits 27-28), LS
    setup_td.header = (USB_PID_SETUP) | ((uint32_t)dev_addr << 8) | (0 << 15) | (0 << 19) | ((8 - 1) << 21);
    setup_td.buffer_ptr = (uint32_t)setup;

    // Stage 2: DATA Stage TD
    if (data_len > 0) {
        data_td.link_ptr = ((uint32_t)&status_td) | UHCI_PTR_DEPTH_FIRST;
        data_td.status = (1 << 23) | (3 << 27) | ls_bit; // Active = 1 (bit 23), C_ERR = 3 (bits 27-28), LS
        uint8_t pid = is_in ? USB_PID_IN : USB_PID_OUT;
        data_td.header = (pid) | ((uint32_t)dev_addr << 8) | (0 << 15) | (1 << 19) | (((uint32_t)(data_len - 1)) << 21);
        data_td.buffer_ptr = (uint32_t)data_buf;
    }

    // Stage 3: STATUS Stage TD
    status_td.link_ptr = UHCI_PTR_TERMINATE;
    status_td.status = (1 << 23) | (3 << 27) | ls_bit; // Active = 1 (bit 23), C_ERR = 3 (bits 27-28), LS
    uint8_t status_pid = (data_len > 0 && is_in) ? USB_PID_OUT : USB_PID_IN;
    status_td.header = (status_pid) | ((uint32_t)dev_addr << 8) | (0 << 15) | (1 << 19) | (0x7FF << 21);
    status_td.buffer_ptr = 0;

    // Queue Head (QH) Link
    control_qh.head_ptr = UHCI_PTR_TERMINATE;
    control_qh.element_ptr = (uint32_t)&setup_td;

    // Link QH across ALL 1024 entries in Frame List
    uint32_t *frame_list = uhci_get_frame_list();
    if (frame_list) {
        uint32_t qh_ptr = ((uint32_t)&control_qh) | UHCI_PTR_QH;
        for (int i = 0; i < 1024; i++) {
            frame_list[i] = qh_ptr;
        }
    }

    // Hardware Completion Poll Loop
    int timeout = 200000;
    int transfer_completed = 0;

    while (timeout > 0) {
        uint16_t usbsts = inw(io_base + UHCI_REG_USBSTS);

        // Check if status_td Active Bit (Bit 23) is cleared OR Hardware USBINT flag (Bit 0) is set
        if (((status_td.status & (1 << 23)) == 0) || (usbsts & (UHCI_STS_USBINT | UHCI_STS_ERROR))) {
            transfer_completed = 1;
            break;
        }

        pit_delay_ms(1);
        timeout--;
    }

    // Unlink Queue Head from Frame List
    if (frame_list) {
        for (int i = 0; i < 1024; i++) {
            frame_list[i] = UHCI_PTR_TERMINATE;
        }
    }

    if (!transfer_completed || (status_td.status & (1 << 23))) {
        printf("[USB Enum Debug] Control Transfer Timed Out!\n");
        printf("  SETUP Stage TD Status  : 0x%08X (ActLen: 0x%X)\n", setup_td.status, setup_td.status & 0x7FF);
        if (data_len > 0) {
            printf("  DATA Stage TD Status   : 0x%08X (ActLen: 0x%X)\n", data_td.status, data_td.status & 0x7FF);
        }
        printf("  STATUS Stage TD Status : 0x%08X (ActLen: 0x%X)\n", status_td.status, status_td.status & 0x7FF);
        printf("  USBSTS Register Value  : 0x%04X\n", inw(io_base + UHCI_REG_USBSTS));
        return 0;
    }

    return 1;
}

void usb_print_device_info(const usb_device_descriptor_t *desc) {
    printf("====================================================\n");
    printf("           USB Device Descriptor Info               \n");
    printf("====================================================\n");
    printf("  bLength            : %d bytes\n", desc->bLength);
    printf("  bDescriptorType    : 0x%X (DEVICE)\n", desc->bDescriptorType);
    printf("  bcdUSB             : 0x%04X (USB %d.%d)\n", desc->bcdUSB, (desc->bcdUSB >> 8) & 0xFF, (desc->bcdUSB >> 4) & 0x0F);
    printf("  bDeviceClass       : 0x%02X ", desc->bDeviceClass);
    
    if (desc->bDeviceClass == 0x00) printf("(Defined at Interface Level)\n");
    else if (desc->bDeviceClass == 0x08) printf("(Mass Storage Flash Drive)\n");
    else if (desc->bDeviceClass == 0x03) printf("(HID Human Interface Device)\n");
    else printf("(Other Class)\n");

    printf("  bDeviceSubClass    : 0x%02X\n", desc->bDeviceSubClass);
    printf("  bDeviceProtocol    : 0x%02X\n", desc->bDeviceProtocol);
    printf("  bMaxPacketSize0    : %d bytes\n", desc->bMaxPacketSize0);
    printf("  idVendor           : 0x%04X\n", desc->idVendor);
    printf("  idProduct          : 0x%04X\n", desc->idProduct);
    printf("  bcdDevice          : 0x%04X\n", desc->bcdDevice);
    printf("  bNumConfigurations : %d\n", desc->bNumConfigurations);
    printf("====================================================\n");
}

int usb_enumerate_device(uint16_t port_num) {
    uint16_t io_base = uhci_get_io_base();
    if (io_base == 0) return 0;

    uint16_t port_reg = (port_num == 1) ? (io_base + UHCI_REG_PORTSC1) : (io_base + UHCI_REG_PORTSC2);

    if (!usb_reset_port(port_reg)) return 0;

    printf("[USB Enum] Sending GET_DESCRIPTOR (18 Bytes) on Address 0...\n");
    memset(&setup_pkt, 0, sizeof(usb_setup_packet_t));
    setup_pkt.bmRequestType = 0x80;            
    setup_pkt.bRequest = 0x06;                 
    setup_pkt.wValue = 0x0100;                 
    setup_pkt.wIndex = 0x0000;
    setup_pkt.wLength = 18;                    

    memset(descriptor_buffer, 0, sizeof(descriptor_buffer));

    if (!uhci_exec_control_transfer(0, &setup_pkt, descriptor_buffer, 18, 1, port_reg)) {
        printf("[USB Enum] Failed to retrieve Device Descriptor on Address 0.\n");
        return 0;
    }

    usb_device_descriptor_t *dev_desc = (usb_device_descriptor_t *)descriptor_buffer;
    usb_print_device_info(dev_desc);

    printf("[USB Enum] Assigning Device Address 1 via SET_ADDRESS...\n");
    memset(&setup_pkt, 0, sizeof(usb_setup_packet_t));
    setup_pkt.bmRequestType = 0x00;            
    setup_pkt.bRequest = 0x05;                 
    setup_pkt.wValue = 0x0001;                 
    setup_pkt.wIndex = 0x0000;
    setup_pkt.wLength = 0;                     

    if (!uhci_exec_control_transfer(0, &setup_pkt, NULL, 0, 0, port_reg)) {
        printf("[USB Enum] Failed to set device address to 1.\n");
        return 0;
    }

    pit_delay_ms(10);
    printf("[USB Enum] Device Enumerate Success! Device is now Active on Address 1.\n");

    usb_set_port_enumerated(port_num, 1);

    usb_storage_init(1, port_reg);

    return 1;
}
