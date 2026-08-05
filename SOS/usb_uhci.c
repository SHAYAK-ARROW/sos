#include "usb_uhci.h"
#include "usb_enum.h"
#include "io.h"
#include "screen.h"
#include "libc/stdio.h"
#include "libc/string.h"

static uint16_t uhci_io_base = 0;

// Frame List (1024 entries, 4KB aligned)
static uint32_t uhci_frame_list[1024] __attribute__((aligned(4096)));

uint16_t uhci_get_io_base(void) {
    return uhci_io_base;
}

uint32_t* uhci_get_frame_list(void) {
    return uhci_frame_list;
}

static void delay_ms(int ms) {
    for (int i = 0; i < ms * 1000; i++) {
        io_wait();
    }
}

void uhci_dump_registers(uint16_t *out_usbsts, uint16_t *out_usbcmd) {
    if (uhci_io_base != 0) {
        if (out_usbsts) *out_usbsts = inw(uhci_io_base + UHCI_REG_USBSTS);
        if (out_usbcmd) *out_usbcmd = inw(uhci_io_base + UHCI_REG_USBCMD);
    } else {
        if (out_usbsts) *out_usbsts = 0;
        if (out_usbcmd) *out_usbcmd = 0;
    }
}

// Proactive & Reactive Controller Recovery (Checks both USBCMD RS bit & USBSTS HCHalted bit)
int uhci_ensure_running(void) {
    if (uhci_io_base == 0) return 0;

    uint16_t usbsts = inw(uhci_io_base + UHCI_REG_USBSTS);
    uint16_t usbcmd = inw(uhci_io_base + UHCI_REG_USBCMD);

    // Check BOTH: HCHalted bit in USBSTS OR Run/Stop bit not set in USBCMD
    if ((usbsts & UHCI_STS_HCHALTED) || ((usbcmd & UHCI_CMD_RS) == 0)) {
        printf("[USB Recovery] Controller stopped or halted (USBCMD=0x%04X, USBSTS=0x%04X) — re-asserting Run/Stop.\n",
               usbcmd, usbsts);

        // Clear all Write-1-to-Clear USBSTS status flags
        outw(uhci_io_base + UHCI_REG_USBSTS, 0x00FF);

        // Re-assert Run/Stop bit & Max Packet Size
        outw(uhci_io_base + UHCI_REG_USBCMD, UHCI_CMD_RS | UHCI_CMD_MAXP);
        delay_ms(10);

        uint16_t new_cmd = inw(uhci_io_base + UHCI_REG_USBCMD);
        uint16_t new_sts = inw(uhci_io_base + UHCI_REG_USBSTS);

        if ((new_cmd & UHCI_CMD_RS) && ((new_sts & UHCI_STS_HCHALTED) == 0)) {
            printf("[USB Recovery] Controller successfully restored to RUNNING state! (USBCMD=0x%04X, USBSTS=0x%04X)\n",
                   new_cmd, new_sts);
            return 1;
        } else {
            printf("[USB Recovery] Warning: Controller failed to resume (USBCMD=0x%04X, USBSTS=0x%04X).\n",
                   new_cmd, new_sts);
            return 0;
        }
    }

    return 1; // Already running
}

int uhci_recover_if_halted(void) {
    return uhci_ensure_running();
}

int uhci_init(pci_device_t *dev) {
    printf("[UHCI Driver] Initializing UHCI USB Host Controller at PCI %d:%d.%d...\n",
           dev->bus, dev->slot, dev->func);

    uint32_t bar4 = pci_read_config_dword(dev->bus, dev->slot, dev->func, 0x20);
    
    if ((bar4 & 0x01) == 0) {
        printf("[UHCI Driver] Error: BAR4 is Memory mapped, expected I/O space.\n");
        return 0;
    }

    uhci_io_base = (uint16_t)(bar4 & 0xFFFC);
    printf("[UHCI Driver] UHCI Controller I/O Base Address (BAR4): 0x%X\n", uhci_io_base);

    uint32_t pci_cmd = pci_read_config_dword(dev->bus, dev->slot, dev->func, 0x04);
    pci_cmd |= 0x05;
    pci_write_config_dword(dev->bus, dev->slot, dev->func, 0x04, pci_cmd);

    outw(uhci_io_base + UHCI_REG_USBCMD, UHCI_CMD_GRESET);
    delay_ms(50);

    outw(uhci_io_base + UHCI_REG_USBCMD, 0x0000);
    delay_ms(10);

    outw(uhci_io_base + UHCI_REG_USBCMD, UHCI_CMD_HCRESET);
    
    int timeout = 1000;
    while ((inw(uhci_io_base + UHCI_REG_USBCMD) & UHCI_CMD_HCRESET) && --timeout > 0) {
        delay_ms(1);
    }

    outw(uhci_io_base + UHCI_REG_USBINTR, 0x0000);
    outw(uhci_io_base + UHCI_REG_USBSTS, 0x00FF);

    for (int i = 0; i < 1024; i++) {
        uhci_frame_list[i] = UHCI_PTR_TERMINATE;
    }

    outl(uhci_io_base + UHCI_REG_FRBASEADD, (uint32_t)uhci_frame_list);
    outw(uhci_io_base + UHCI_REG_FRNUM, 0x0000);

    outw(uhci_io_base + UHCI_REG_USBCMD, UHCI_CMD_RS | UHCI_CMD_MAXP);

    uint16_t status = inw(uhci_io_base + UHCI_REG_USBSTS);
    if (status & UHCI_STS_HCHALTED) {
        printf("[UHCI Driver] Warning: Controller failed to start.\n");
    } else {
        printf("[UHCI Driver] UHCI Host Controller is RUNNING successfully!\n");
    }

    uhci_check_ports();

    return 1;
}

void uhci_check_ports(void) {
    if (uhci_io_base == 0) return;

    printf("[UHCI Driver] Checking Root Hub Ports Status (PORTSC1 & PORTSC2)...\n");

    uint16_t ports[2] = {
        uhci_io_base + UHCI_REG_PORTSC1,
        uhci_io_base + UHCI_REG_PORTSC2
    };

    for (int i = 0; i < 2; i++) {
        uint16_t portsc = inw(ports[i]);
        printf("  Port %d (I/O 0x%X): Status = 0x%X -> ", i + 1, ports[i], portsc);

        if (portsc & UHCI_PORTSC_CCS) {
            if (usb_is_port_enumerated(i + 1)) {
                printf("[DEVICE CONNECTED & ACTIVE (Address 1)]\n");
            } else {
                printf("[NEW DEVICE CONNECTED - STARTING ENUMERATION]\n");
                usb_enumerate_device(i + 1);
            }
        } else {
            printf("[NO DEVICE CONNECTED]\n");
            usb_set_port_enumerated(i + 1, 0);
        }

        if (portsc & UHCI_PORTSC_CSC) {
            outw(ports[i], portsc | UHCI_PORTSC_CSC);
        }
    }
}
