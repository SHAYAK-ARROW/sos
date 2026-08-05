#ifndef USB_UHCI_H
#define USB_UHCI_H

#include <stdint.h>
#include "pci.h"

// ============================================================================
// UHCI Register Offsets (I/O Space relative to BAR4)
// ============================================================================
#define UHCI_REG_USBCMD      0x00 // USB Command Register (16-bit)
#define UHCI_REG_USBSTS      0x02 // USB Status Register (16-bit)
#define UHCI_REG_USBINTR     0x04 // USB Interrupt Enable Register (16-bit)
#define UHCI_REG_FRNUM       0x06 // Frame Number Register (16-bit)
#define UHCI_REG_FRBASEADD   0x08 // Frame List Base Address Register (32-bit)
#define UHCI_REG_SOFMOD      0x0C // Start of Frame Modify Register (8-bit)
#define UHCI_REG_PORTSC1     0x10 // Port 1 Status/Control Register (16-bit)
#define UHCI_REG_PORTSC2     0x12 // Port 2 Status/Control Register (16-bit)

// ============================================================================
// USBCMD Register Bits
// ============================================================================
#define UHCI_CMD_RS          (1 << 0) // Run / Stop (1 = Run, 0 = Stop)
#define UHCI_CMD_HCRESET     (1 << 1) // Host Controller Reset
#define UHCI_CMD_GRESET      (1 << 2) // Global Reset
#define UHCI_CMD_EGSM        (1 << 3) // Enter Global Suspend Mode
#define UHCI_CMD_FGR         (1 << 4) // Force Global Resume
#define UHCI_CMD_SWDBG       (1 << 5) // Software Debug
#define UHCI_CMD_CF          (1 << 6) // Configure Flag
#define UHCI_CMD_MAXP        (1 << 7) // Max Packet Size (0 = 32 bytes, 1 = 64 bytes)

// ============================================================================
// USBSTS Register Bits
// ============================================================================
#define UHCI_STS_USBINT      (1 << 0) // USB Interrupt
#define UHCI_STS_ERROR       (1 << 1) // USB Error Interrupt
#define UHCI_STS_RD          (1 << 2) // Resume Detect
#define UHCI_STS_HSE         (1 << 3) // Host System Error
#define UHCI_STS_HCPROCESS   (1 << 4) // Host Controller Process Error
#define UHCI_STS_HCHALTED    (1 << 5) // Host Controller Halted

// ============================================================================
// PORTSC Register Bits
// ============================================================================
#define UHCI_PORTSC_CCS      (1 << 0) // Current Connect Status (1 = Device Attached)
#define UHCI_PORTSC_CSC      (1 << 1) // Connect Status Change
#define UHCI_PORTSC_PE       (1 << 2) // Port Enable / Disable
#define UHCI_PORTSC_PEC      (1 << 3) // Port Enable/Disable Change
#define UHCI_PORTSC_LSDA     (1 << 8) // Low Speed Device Attached (1 = Low Speed, 0 = Full Speed)
#define UHCI_PORTSC_PR       (1 << 9) // Port Reset (write 1 to reset port)

// ============================================================================
// Frame List / Queue Pointer Flags
// ============================================================================
#define UHCI_PTR_TERMINATE   0x00000001 // Bit 0 = 1 (Terminate / Invalid)
#define UHCI_PTR_QH          0x00000002 // Bit 1 = 1 (Queue Head)
#define UHCI_PTR_DEPTH_FIRST 0x00000004 // Bit 2 = 1 (Vf / Depth First Select)

// Driver Functions
int uhci_init(pci_device_t *dev);
void uhci_check_ports(void);
uint16_t uhci_get_io_base(void);
uint32_t* uhci_get_frame_list(void);

// Diagnostic & Recovery Functions
void uhci_dump_registers(uint16_t *out_usbsts, uint16_t *out_usbcmd);
int uhci_recover_if_halted(void);
int uhci_ensure_running(void);

#endif
