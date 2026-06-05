#ifndef USB_H
#define USB_H

/*
 * usb.h — xHCI shared state exported to usb_hid_kbd.c
 *
 * usb.c owns the xHCI controller, command ring, and event ring.
 * usb_hid_kbd.c shares them instead of re-initialising.
 */

#include <stdint.h>

/* ── Ring entry types ───────────────────────────────────────────────── */
typedef struct {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) xhci_trb_t;

typedef struct {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t size;
    uint32_t rsvd;
} __attribute__((packed)) xhci_erst_t;

/* ── Exported MMIO bases (set during usb_init) ──────────────────────── */
extern uint32_t g_xhci_op_base;
extern uint32_t g_xhci_rt_base;
extern uint32_t g_xhci_db_base;
extern uint32_t g_xhci_num_ports;

/*
 * g_ctx_dwords — context entry size in 32-bit words.
 * 8  = 32-byte contexts (HCCPARAMS1[CSZ] = 0).
 * 16 = 64-byte contexts (HCCPARAMS1[CSZ] = 1).
 * All context offset calculations in usb_hid_kbd.c must use this value.
 */
extern uint32_t g_ctx_dwords;

/* ── Exported ring state ────────────────────────────────────────────── */
extern xhci_trb_t  *g_cmd_ring;
extern xhci_trb_t  *g_evt_ring;
extern xhci_erst_t *g_erst;
extern uint64_t    *g_dcbaa;

extern uint32_t g_cmd_idx;
extern uint32_t g_cmd_cycle;
extern uint32_t g_evt_idx;
extern uint32_t g_evt_cycle;

/* ── Shared xHCI operations ─────────────────────────────────────────── */
int  xhci_wait_event(uint32_t want_type, uint32_t timeout_ms);
int  xhci_send_cmd(uint32_t type, uint32_t p0, uint32_t p1,
                   uint32_t status_field, uint32_t slot);
void xhci_ring_doorbell(uint32_t slot, uint32_t target);

/*
 * xhci_control_transfer — standards-compliant EP0 control transfer.
 *
 * Sets TRT correctly for all three transfer directions:
 *   wLen == 0              → TRT = 0 (No Data)
 *   wLen >  0 && is_in    → TRT = 3 (IN  Data)
 *   wLen >  0 && !is_in   → TRT = 2 (OUT Data)
 *
 * slot    : xHCI slot ID that owns the EP0 ring being used.
 * bmRT    : bmRequestType byte.
 * bReq    : bRequest byte.
 * wVal    : wValue.
 * wIdx    : wIndex.
 * wLen    : wLength.
 * buf     : data buffer (may be NULL when wLen == 0).
 * is_in   : 1 for device→host data stage, 0 for host→device.
 *
 * Uses the shared ep0_ring owned by usb.c (mass-storage slot).
 * usb_hid_kbd.c has its own EP0 ring and its own wrapper that calls
 * this signature — see hid_control_xfer() there.
 *
 * Returns 0 on success, -1 on timeout or controller error.
 */
int xhci_control_transfer(uint8_t bmRT, uint8_t bReq, uint16_t wVal,
                           uint16_t wIdx, uint16_t wLen, void *buf, int is_in);

/* ── Occupied-port table (index = 0-based port number) ─────────────── */
#define XHCI_MAX_PORTS 32
extern uint8_t g_xhci_occupied_ports[XHCI_MAX_PORTS];

/* ── Mass storage public API ────────────────────────────────────────── */
int      usb_init(void);
int      usb_read_sector(uint32_t lba, uint8_t *buf);
int      usb_write_sector(uint32_t lba, uint8_t *buf);
int      usb_disk_present(void);
uint32_t usb_part2_lba(void);
uint32_t usb_part2_size(void);

#endif /* USB_H */
