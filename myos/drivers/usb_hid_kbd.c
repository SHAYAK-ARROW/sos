/*
 * usb_hid_kbd.c — USB HID Boot-Protocol Keyboard Driver
 *
 * Enumeration sequence per USB HID spec §4.4 and xHCI spec §4:
 *   1. Port reset
 *   2. Enable Slot
 *   3. Address Device (EP0 only)
 *   4. GET DESCRIPTOR (Device)        — bMaxPacketSize0, bDeviceClass
 *   5. GET DESCRIPTOR (Configuration) — full descriptor walk
 *   6. Find Boot Keyboard interface   — class=3, subclass=1, protocol=1
 *   7. Find Interrupt IN endpoint     — actual address and wMaxPacketSize
 *   8. SET CONFIGURATION
 *   9. SET PROTOCOL (Boot)
 *  10. SET IDLE (0)
 *  11. Configure EP                   — interrupt IN endpoint
 *
 * All endpoint addresses and packet sizes come from descriptors.
 * Nothing is hardcoded.
 * Context offsets use g_ctx_dwords to support both 32- and 64-byte formats.
 */

#include "usb_hid_kbd.h"
#include "usb.h"
#include "../mm/pmm.h"
#include <stdint.h>

extern void terminal_putchar(char c);
extern void terminal_writeline(const char *s);
extern void terminal_write(const char *s);

#define MMIO_R32(a)    (*(volatile uint32_t *)(uintptr_t)(a))
#define MMIO_W32(a, v) ((*(volatile uint32_t *)(uintptr_t)(a)) = (v))

/* ── TRB type constants (local, matching usb.c) ─────────────────────── */
#define TRB_NORMAL          1
#define TRB_SETUP           2
#define TRB_DATA            3
#define TRB_STATUS          4
#define TRB_LINK            6
#define TRB_ENABLE_SLOT     9
#define TRB_DISABLE_SLOT    10
#define TRB_ADDRESS_DEVICE  11
#define TRB_CONFIGURE_EP    12
#define TRB_TRANSFER_EVT    32
#define TRB_CMD_COMPLETION  33
#define CC_SUCCESS          1

/* ── USB request constants ──────────────────────────────────────────── */
#define USB_RT_DEV_TO_HOST  0x80
#define USB_RT_HOST_TO_DEV  0x00
#define USB_RT_CLASS        0x20
#define USB_RT_IFACE        0x01
#define USB_REQ_GET_DESC    0x06
#define USB_REQ_SET_CONFIG  0x09
#define USB_REQ_SET_PROTO   0x0B
#define USB_REQ_SET_IDLE    0x0A
#define USB_PROTO_BOOT      0

/* ── USB descriptor type constants ─────────────────────────────────── */
#define DESC_CONFIGURATION  0x02
#define DESC_INTERFACE      0x04
#define DESC_ENDPOINT       0x05

/* ── HID class identifiers ──────────────────────────────────────────── */
#define HID_CLASS           0x03
#define HID_SUBCLASS_BOOT   0x01
#define HID_PROTO_KEYBOARD  0x01

/* ── Modifier mask ──────────────────────────────────────────────────── */
#define MOD_SHIFT  ((1u << 1) | (1u << 5))

typedef xhci_trb_t trb_t;

/* ── Per-keyboard ring state ─────────────────────────────────────────── */
static trb_t    *kbd_ep0_ring = 0;
static trb_t    *kbd_in_ring  = 0;
static uint32_t *kbd_dev_ctx  = 0;
static uint32_t *kbd_in_ctx   = 0;
static uint8_t  *kbd_desc_buf = 0;  /* 512 bytes for config descriptor */

static uint32_t g_ep0_idx   = 0;
static uint32_t g_ep0_cycle = 1;
static uint32_t g_in_idx    = 0;
static uint32_t g_in_cycle  = 1;

static uint32_t g_kbd_slot   = 0;
static uint8_t  g_kbd_ep_addr = 0;    /* actual endpoint address from descriptor */
static uint8_t  g_kbd_ep_dci  = 3;    /* DCI computed from ep_addr              */
static uint16_t g_kbd_ep_mps  = 8;    /* wMaxPacketSize from descriptor         */
static int      g_kbd_ready   = 0;
static int      g_caps_lock   = 0;

/* ── HID scan-code to ASCII tables ─────────────────────────────────── */
static const char hid_to_ascii[0x80] = {
    0, 0, 0, 0,
    'a','b','c','d','e','f','g','h','i','j','k','l','m',
    'n','o','p','q','r','s','t','u','v','w','x','y','z',
    '1','2','3','4','5','6','7','8','9','0',
    '\n', 27, '\b', '\t', ' ',
    '-','=','[',']','\\', 0, ';','\'','`',',','.','/',
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0, '/', '*', '-', '+', '\n',
    '1','2','3','4','5','6','7','8','9','0','.'
};

static const char hid_to_ascii_shift[0x80] = {
    0, 0, 0, 0,
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    '\n', 27, '\b', '\t', ' ',
    '_','+','{','}','|', 0, ':','"','~','<','>','?',
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static void hid_delay(uint32_t us) {
    volatile uint32_t n = us * 200;
    while (n--) __asm__ volatile ("nop");
}

static void memclr_hid(void *ptr, uint32_t bytes) {
    uint8_t *p = (uint8_t *)ptr;
    for (uint32_t i = 0; i < bytes; i++) p[i] = 0;
}

/* ── EP0 ring helpers (HID keyboard's own EP0 ring) ─────────────────── */
static void ep0_enqueue(uint32_t p0, uint32_t p1, uint32_t st, uint32_t ctrl) {
    /* ctrl is passed with bit 0 clear; we set the correct cycle bit here */
    ctrl = (ctrl & ~1u) | (g_ep0_cycle & 1u);
    kbd_ep0_ring[g_ep0_idx].param_lo = p0;
    kbd_ep0_ring[g_ep0_idx].param_hi = p1;
    kbd_ep0_ring[g_ep0_idx].status   = st;
    kbd_ep0_ring[g_ep0_idx].control  = ctrl;
    g_ep0_idx++;
    if (g_ep0_idx >= 63) {
        kbd_ep0_ring[63].param_lo = (uint32_t)(uintptr_t)kbd_ep0_ring;
        kbd_ep0_ring[63].param_hi = 0;
        kbd_ep0_ring[63].status   = 0;
        kbd_ep0_ring[63].control  = (TRB_LINK << 10) | 0x02u | g_ep0_cycle;
        g_ep0_idx    = 0;
        g_ep0_cycle ^= 1;
    }
}

/*
 * hid_control_xfer — EP0 control transfer on the keyboard's own EP0 ring.
 *
 * TRT is set correctly for all three transfer types:
 *   wLen == 0              → TRT = 0 (No Data)
 *   wLen >  0 && is_in    → TRT = 3 (IN  Data)
 *   wLen >  0 && !is_in   → TRT = 2 (OUT Data)
 */
static int hid_control_xfer(uint32_t slot,
                              uint8_t bm_req, uint8_t b_req,
                              uint16_t w_val, uint16_t w_idx,
                              uint16_t w_len, void *data, int is_in) {
    uint32_t setup_lo  = (uint32_t)bm_req | ((uint32_t)b_req << 8)
                       | ((uint32_t)w_val << 16);
    uint32_t setup_hi  = (uint32_t)w_idx  | ((uint32_t)w_len << 16);
    uint32_t trt       = (w_len == 0) ? 0u : (is_in ? 3u : 2u);
    ep0_enqueue(setup_lo, setup_hi, 8u,
                (TRB_SETUP << 10) | (1u << 6) | (trt << 16));
    if (w_len > 0 && data) {
        ep0_enqueue((uint32_t)(uintptr_t)data, 0, (uint32_t)w_len,
                    (TRB_DATA << 10) | (1u << 5) | (is_in ? (1u << 16) : 0u));
    }
    /* Status stage: direction opposite to data stage; IN for no-data */
    uint32_t sdir = (w_len == 0 || !is_in) ? (1u << 16) : 0u;
    ep0_enqueue(0, 0, 0, (TRB_STATUS << 10) | (1u << 5) | sdir);
    xhci_ring_doorbell(slot, 1);
    if (w_len > 0 && data) {
        if (xhci_wait_event(TRB_TRANSFER_EVT, 2000) < 0) return -1;
    }
    if (xhci_wait_event(TRB_TRANSFER_EVT, 2000) < 0) return -1;
    return 0;
}

/* ── Memory allocation ───────────────────────────────────────────────── */
static int hid_alloc_rings(void) {
    kbd_ep0_ring = (trb_t    *)pmm_alloc_page();
    kbd_in_ring  = (trb_t    *)pmm_alloc_page();
    kbd_dev_ctx  = (uint32_t *)pmm_alloc_page();
    kbd_in_ctx   = (uint32_t *)pmm_alloc_page();
    kbd_desc_buf = (uint8_t  *)pmm_alloc_page();  /* 4096 bytes — fits any config desc */
    if (!kbd_ep0_ring || !kbd_in_ring || !kbd_dev_ctx
                      || !kbd_in_ctx  || !kbd_desc_buf) {
        terminal_writeline("[USB HID] PMM out of memory.");
        return -1;
    }
    memclr_hid(kbd_ep0_ring, 4096);
    memclr_hid(kbd_in_ring,  4096);
    memclr_hid(kbd_dev_ctx,  4096);
    memclr_hid(kbd_in_ctx,   4096);
    memclr_hid(kbd_desc_buf, 4096);
    g_ep0_idx = 0; g_ep0_cycle = 1;
    g_in_idx  = 0; g_in_cycle  = 1;
    return 0;
}

/*
 * hid_build_input_ctx — build an xHCI Input Context for Address Device
 * or Configure EP.
 *
 * Uses g_ctx_dwords (exported from usb.c) so the context entry stride
 * is correct for both 32-byte (g_ctx_dwords=8) and 64-byte
 * (g_ctx_dwords=16) context formats.
 *
 * For Address Device  (ep_addr == 0):  sets slot + EP0 context.
 * For Configure EP    (ep_addr != 0):  sets slot + interrupt-IN context.
 *
 * Parameters:
 *   port       : 1-based port number (matches xHCI Root Hub Port Number)
 *   speed      : xHCI speed code (1=Full,2=Low,3=High,4=Super)
 *   ep_addr    : 0 for EP0 setup, else the endpoint's bEndpointAddress
 *   max_packet : wMaxPacketSize (for EP0) or interrupt EP wMaxPacketSize
 *   ep_dci     : DCI for the endpoint (ignored when ep_addr==0)
 */
static void hid_build_input_ctx(uint8_t port, uint8_t speed,
                                 uint8_t ep_addr, uint16_t max_packet,
                                 uint8_t ep_dci) {
    memclr_hid(kbd_in_ctx, 4096);

    if (ep_addr == 0) {
        /* Address Device: enable slot context (A0) and EP0 context (A1) */
        kbd_in_ctx[0] = 0;
        kbd_in_ctx[1] = (1u << 0) | (1u << 1);

        /* Slot context: offset = 1 * g_ctx_dwords */
        uint32_t *slot_ctx = &kbd_in_ctx[g_ctx_dwords];
        slot_ctx[0] = ((uint32_t)speed << 20) | (1u << 27);
        slot_ctx[1] = (uint32_t)port << 16;
        slot_ctx[2] = 0;
        slot_ctx[3] = 0;

        /* EP0 context: offset = 2 * g_ctx_dwords */
        uint32_t *ep0_ctx = &kbd_in_ctx[g_ctx_dwords * 2];
        ep0_ctx[0] = 0u;
        /* EP type 4 = Control Bidirectional, CErr=3 */
        ep0_ctx[1] = (3u << 1) | (4u << 3) | ((uint32_t)max_packet << 16);
        ep0_ctx[2] = (uint32_t)(uintptr_t)kbd_ep0_ring | 1u;
        ep0_ctx[3] = 0;
    } else {
        /* Configure EP: enable slot context (A0) and interrupt-IN (A<ep_dci>) */
        kbd_in_ctx[0] = 0;
        kbd_in_ctx[1] = (1u << 0) | (1u << ep_dci);

        /* Slot context: Context Entries field = ep_dci */
        uint32_t *slot_ctx = &kbd_in_ctx[g_ctx_dwords];
        slot_ctx[0] = ((uint32_t)speed << 20) | ((uint32_t)ep_dci << 27);
        slot_ctx[1] = (uint32_t)port << 16;
        slot_ctx[2] = 0;
        slot_ctx[3] = 0;

        /* Interrupt IN endpoint context: offset = (1 + ep_dci) * g_ctx_dwords */
        uint32_t *ep_ctx = &kbd_in_ctx[g_ctx_dwords * (1u + (uint32_t)ep_dci)];
        /* EP type 7 = Interrupt IN, CErr=3 */
        ep_ctx[0] = (0x06u << 16);   /* Interval = 6 → 2^(6-1) = 32 microframes = 4ms */
        ep_ctx[1] = (3u << 1) | (7u << 3) | ((uint32_t)max_packet << 16);
        ep_ctx[2] = (uint32_t)(uintptr_t)kbd_in_ring | 1u;
        ep_ctx[3] = 0;
        ep_ctx[4] = (uint32_t)max_packet;
    }
}

/* ── Device Descriptor (packed, no padding) ─────────────────────────── */
typedef struct {
    uint8_t  bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} __attribute__((packed)) usb_dev_desc_t;

static usb_dev_desc_t g_dev_desc __attribute__((aligned(4)));

/*
 * hid_try_port — attempt to enumerate a HID boot keyboard on a given port.
 *
 * port : 1-based port number (HID driver uses 1-based, MMIO offset = (port-1)*0x10)
 *
 * Returns 0 if a boot keyboard was found and configured.
 * Returns -1 if this port is not a boot keyboard (tries are silent).
 */
static int hid_try_port(uint8_t port) {
    /* Skip ports already used by mass storage.
     * g_xhci_occupied_ports is 0-based; port is 1-based. */
    if (port >= 1 && port <= XHCI_MAX_PORTS
                  && g_xhci_occupied_ports[port - 1])
        return -1;

    uint32_t portsc_addr = g_xhci_op_base + 0x400 + 0x10 * (uint32_t)(port - 1);
    uint32_t portsc      = MMIO_R32(portsc_addr);

    /* CCS (bit 0) must be set — device connected */
    if (!(portsc & 0x01u)) return -1;

    /* Read speed before reset */
    uint8_t speed = (uint8_t)((portsc >> 10) & 0x0Fu);
    if (speed == 0) speed = 3;  /* default to High Speed if unreported */

    /* Port reset */
    MMIO_W32(portsc_addr, (portsc & 0x0FFFFE0Fu) | (1u << 4));
    hid_delay(50000);   /* 50 ms — USB spec requires ≥ 10 ms reset pulse */
    for (int t = 0; t < 500; t++) {
        portsc = MMIO_R32(portsc_addr);
        if (!(portsc & (1u << 4))) break;
        hid_delay(1000);
    }

    /* Clear change bits (CSC, PRC, PLC, CEC) */
    MMIO_W32(portsc_addr, MMIO_R32(portsc_addr)
             | (1u << 17) | (1u << 21) | (1u << 22) | (1u << 23));
    hid_delay(10000);

    /* PED (Port Enabled, bit 1) must be set after reset */
    int ped = 0;
    for (int t = 0; t < 200; t++) {
        portsc = MMIO_R32(portsc_addr);
        if (portsc & 0x02u) { ped = 1; break; }
        hid_delay(1000);
    }
    if (!ped) return -1;

    /* Re-read speed after reset (may change) */
    speed = (uint8_t)((portsc >> 10) & 0x0Fu);
    if (speed == 0) speed = 3;

    /* EP0 max packet size by speed (xHCI Table 157):
     * 1 = Full Speed  → 8, 16, 32, or 64; use 8 as conservative initial value,
     *                    updated below from bMaxPacketSize0.
     * 2 = Low Speed   → 8
     * 3 = High Speed  → 64
     * 4 = SuperSpeed  → 512  */
    uint16_t ep0_max;
    if      (speed >= 4) ep0_max = 512u;
    else if (speed == 3) ep0_max = 64u;
    else                 ep0_max = 8u;

    /* ── Enable Slot ─────────────────────────────────────────────────── */
    int slot = xhci_send_cmd(TRB_ENABLE_SLOT, 0, 0, 0, 0);
    if (slot <= 0 || slot > 255) return -1;
    g_kbd_slot = (uint32_t)slot;

    /* Assign device context for this slot */
    g_dcbaa[slot] = (uint64_t)(uintptr_t)kbd_dev_ctx;

    /* ── Address Device (EP0 only) ───────────────────────────────────── */
    hid_build_input_ctx(port, speed, 0, ep0_max, 0);
    if (xhci_send_cmd(TRB_ADDRESS_DEVICE,
                      (uint32_t)(uintptr_t)kbd_in_ctx, 0, 0,
                      (uint32_t)slot) < 0) goto fail;

    /* ── GET DESCRIPTOR (Device) ─────────────────────────────────────── */
    memclr_hid(&g_dev_desc, sizeof(g_dev_desc));
    if (hid_control_xfer((uint32_t)slot,
                          USB_RT_DEV_TO_HOST, USB_REQ_GET_DESC,
                          0x0100, 0, 18, &g_dev_desc, 1) < 0) goto fail;

    /* Accept devices where the class is defined at device level (0x03)
     * or deferred to the interface (0x00).  Also accept composite devices
     * (0xEF) which declare the HID class at the interface level.
     * Reject mass storage (0x08) and hub (0x09) class devices. */
    if (g_dev_desc.bDeviceClass == 0x08 || g_dev_desc.bDeviceClass == 0x09)
        goto fail;

    /* Update EP0 max packet from Device Descriptor (Full Speed only;
     * High Speed is always 64, SuperSpeed always 512). */
    if (speed < 3 && g_dev_desc.bMaxPacketSize0 > 0)
        ep0_max = g_dev_desc.bMaxPacketSize0;

    /* ── GET DESCRIPTOR (Configuration, full) ───────────────────────── */
    memclr_hid(kbd_desc_buf, 4096);
    /* First fetch just the configuration descriptor header (9 bytes)
     * to learn wTotalLength, then fetch the full descriptor. */
    if (hid_control_xfer((uint32_t)slot,
                          USB_RT_DEV_TO_HOST, USB_REQ_GET_DESC,
                          (uint16_t)((DESC_CONFIGURATION << 8) | 0),
                          0, 9, kbd_desc_buf, 1) < 0) goto fail;
    uint16_t total_len = (uint16_t)(((uint16_t)kbd_desc_buf[3] << 8)
                                   | kbd_desc_buf[2]);
    if (total_len < 9) goto fail;
    if (total_len > 4096) total_len = 4096;

    memclr_hid(kbd_desc_buf, 4096);
    if (hid_control_xfer((uint32_t)slot,
                          USB_RT_DEV_TO_HOST, USB_REQ_GET_DESC,
                          (uint16_t)((DESC_CONFIGURATION << 8) | 0),
                          0, total_len, kbd_desc_buf, 1) < 0) goto fail;

    /* ── Parse Configuration Descriptor ─────────────────────────────── */
    /*
     * Walk all descriptors inside the configuration.
     * Track the current interface's class/subclass/protocol.
     * When we find a Boot Keyboard interface (class=3, sub=1, proto=1),
     * set in_boot_kbd_iface = 1.
     * The next Interrupt IN endpoint descriptor in that interface is our target.
     * Record its bEndpointAddress and wMaxPacketSize.
     * A new Interface descriptor resets the search state.
     */
    uint8_t  found_ep_addr = 0;
    uint16_t found_ep_mps  = 8;
    uint8_t  in_boot_kbd_iface = 0;
    uint8_t  cfg_value = kbd_desc_buf[5]; /* bConfigurationValue */

    uint32_t off = 0;
    while (off < (uint32_t)total_len) {
        uint8_t bLen  = kbd_desc_buf[off];
        if (bLen < 2) break;
        if ((uint32_t)(off + bLen) > (uint32_t)total_len) break;
        uint8_t bType = kbd_desc_buf[off + 1];

        if (bType == DESC_INTERFACE && bLen >= 9) {
            uint8_t iclass    = kbd_desc_buf[off + 5];
            uint8_t isubclass = kbd_desc_buf[off + 6];
            uint8_t iprotocol = kbd_desc_buf[off + 7];
            in_boot_kbd_iface = (iclass    == HID_CLASS      &&
                                 isubclass == HID_SUBCLASS_BOOT &&
                                 iprotocol == HID_PROTO_KEYBOARD) ? 1 : 0;
        }

        if (bType == DESC_ENDPOINT && bLen >= 7 && in_boot_kbd_iface) {
            uint8_t  ea   = kbd_desc_buf[off + 2];
            uint8_t  attr = kbd_desc_buf[off + 3];
            uint16_t mps  = (uint16_t)(((uint16_t)kbd_desc_buf[off + 5] << 8)
                                      | kbd_desc_buf[off + 4]);
            /* Interrupt IN endpoint: bit 7 set (IN) and transfer type = 3 */
            if ((ea & 0x80u) && (attr & 0x03u) == 0x03u && found_ep_addr == 0) {
                found_ep_addr = ea;
                found_ep_mps  = (mps > 0 && mps <= 64) ? mps : 8u;
                /* Stop after finding the first boot keyboard interrupt-IN endpoint */
                break;
            }
        }

        off += bLen;
    }

    if (found_ep_addr == 0) {
        /* No Boot Keyboard interrupt IN endpoint found on this device */
        goto fail;
    }

    g_kbd_ep_addr = found_ep_addr;
    g_kbd_ep_mps  = found_ep_mps;
    /* DCI = (endpoint number * 2) + direction_bit (1 for IN) */
    g_kbd_ep_dci  = (uint8_t)(((found_ep_addr & 0x0Fu) * 2u) + 1u);

    /* ── SET CONFIGURATION ───────────────────────────────────────────── */
    if (hid_control_xfer((uint32_t)slot,
                          USB_RT_HOST_TO_DEV, USB_REQ_SET_CONFIG,
                          cfg_value, 0, 0, 0, 0) < 0) goto fail;
    hid_delay(20000);   /* 20 ms for device to activate endpoints */

    /* ── SET PROTOCOL (Boot) ─────────────────────────────────────────── */
    hid_control_xfer((uint32_t)slot,
                     USB_RT_HOST_TO_DEV | USB_RT_CLASS | USB_RT_IFACE,
                     USB_REQ_SET_PROTO, USB_PROTO_BOOT, 0, 0, 0, 0);

    /* ── SET IDLE (0 — report only on change) ────────────────────────── */
    hid_control_xfer((uint32_t)slot,
                     USB_RT_HOST_TO_DEV | USB_RT_CLASS | USB_RT_IFACE,
                     USB_REQ_SET_IDLE, 0, 0, 0, 0, 0);

    /* ── Configure EP (interrupt IN) ────────────────────────────────── */
    hid_build_input_ctx(port, speed, g_kbd_ep_addr, g_kbd_ep_mps, g_kbd_ep_dci);
    if (xhci_send_cmd(TRB_CONFIGURE_EP,
                      (uint32_t)(uintptr_t)kbd_in_ctx, 0, 0,
                      (uint32_t)slot) < 0) goto fail;

    /* ── Set up interrupt IN transfer ring Link TRB ──────────────────── */
    kbd_in_ring[63].param_lo = (uint32_t)(uintptr_t)kbd_in_ring;
    kbd_in_ring[63].param_hi = 0;
    kbd_in_ring[63].status   = 0;
    kbd_in_ring[63].control  = (TRB_LINK << 10) | 0x02u | g_in_cycle;

    terminal_writeline("[USB HID] Keyboard configured (Boot Protocol).");
    g_kbd_ready = 1;
    return 0;

fail:
    xhci_send_cmd(TRB_DISABLE_SLOT, 0, 0, 0, (uint32_t)slot);
    g_kbd_slot = 0;
    return -1;
}

/* ── Public init ─────────────────────────────────────────────────────── */
int usb_hid_kbd_init(void) {
    g_kbd_ready = 0;

    if (!g_xhci_op_base || !g_cmd_ring || !g_evt_ring) {
        terminal_writeline("[USB HID] xHCI not initialised; skipping.");
        return -1;
    }

    if (hid_alloc_rings() < 0) return -1;

    /* Allow ports to settle after mass storage initialisation */
    terminal_writeline("[USB HID] Waiting for ports to settle...");
    hid_delay(1000000);

    /* Clear CSC/PRC/PLC/CEC change bits on all ports for a clean CCS read */
    for (uint8_t p = 1;
         p <= (uint8_t)g_xhci_num_ports && p <= XHCI_MAX_PORTS; p++) {
        uint32_t pr = g_xhci_op_base + 0x400 + 0x10 * (uint32_t)(p - 1);
        uint32_t ps = MMIO_R32(pr);
        MMIO_W32(pr, ps | (1u<<17) | (1u<<21) | (1u<<22) | (1u<<23));
    }
    hid_delay(200000);

    /* Scan ports */
    for (uint8_t p = 1;
         p <= (uint8_t)g_xhci_num_ports && p <= XHCI_MAX_PORTS; p++) {
        uint32_t psc = MMIO_R32(g_xhci_op_base + 0x400 + 0x10*(uint32_t)(p-1));

        /* Build and print port status line */
        char dbg[32];
        const char *hx = "0123456789ABCDEF";
        dbg[0]='P'; dbg[1]='o'; dbg[2]='r'; dbg[3]='t'; dbg[4]=' ';
        dbg[5] = (p >= 10) ? ('0' + p/10) : ' ';
        dbg[6] = '0' + p % 10;
        dbg[7]=':'; dbg[8]=' '; dbg[9]='0'; dbg[10]='x';
        for (int i = 0; i < 8; i++) dbg[11+i] = hx[(psc >> (28 - i*4)) & 0xF];
        dbg[19] = '\0';
        terminal_writeline(dbg);

        if (!(psc & 0x01u)) continue;                       /* CCS=0, skip */
        if (g_xhci_occupied_ports[p - 1]) continue;          /* mass storage port */
        if (hid_try_port(p) == 0) return 0;                  /* found keyboard */
    }

    terminal_writeline("[USB HID] No USB keyboard found on any port.");
    terminal_writeline("Keyboard fallback PS/2.");
    return -1;
}

int usb_hid_kbd_present(void) { return g_kbd_ready; }

/* ── Interrupt IN polling ────────────────────────────────────────────── */
static uint8_t g_kbd_report[8] __attribute__((aligned(16)));
static int     g_trb_pending = 0;

static int hid_poll_report(void) {
    if (!g_trb_pending) {
        memclr_hid(g_kbd_report, 8);
        uint32_t idx = g_in_idx;
        kbd_in_ring[idx].param_lo = (uint32_t)(uintptr_t)g_kbd_report;
        kbd_in_ring[idx].param_hi = 0;
        kbd_in_ring[idx].status   = g_kbd_ep_mps;
        kbd_in_ring[idx].control  = (TRB_NORMAL << 10) | (1u << 5) | g_in_cycle;
        g_in_idx++;
        if (g_in_idx >= 63) {
            kbd_in_ring[63].param_lo = (uint32_t)(uintptr_t)kbd_in_ring;
            kbd_in_ring[63].param_hi = 0;
            kbd_in_ring[63].status   = 0;
            kbd_in_ring[63].control  = (TRB_LINK << 10) | 0x02u | g_in_cycle;
            g_in_idx    = 0;
            g_in_cycle ^= 1;
        }
        xhci_ring_doorbell(g_kbd_slot, (uint32_t)g_kbd_ep_dci);
        g_trb_pending = 1;
    }
    if (xhci_wait_event(TRB_TRANSFER_EVT, 50) >= 0) {
        g_trb_pending = 0;
        return 0;
    }
    return -1;
}

static char hid_report_to_char(const uint8_t *rep) {
    uint8_t mods    = rep[0];
    uint8_t keycode = 0;
    for (int i = 2; i < 8; i++) { if (rep[i]) { keycode = rep[i]; break; } }
    if (!keycode) return 0;
    if (keycode == 0x39) { g_caps_lock ^= 1; return 0; }
    int  shifted = (mods & MOD_SHIFT) != 0;
    char c = (keycode < 0x80)
           ? (shifted ? hid_to_ascii_shift[keycode] : hid_to_ascii[keycode])
           : 0;
    if (g_caps_lock && c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (g_caps_lock && c >= 'A' && c <= 'Z' && !shifted) c = (char)(c + 32);
    return c;
}

char usb_hid_kbd_getchar(void) {
    if (!g_kbd_ready) return 0;
    static uint8_t prev_keycode = 0;
    while (1) {
        while (hid_poll_report() < 0) {}
        uint8_t cur = 0;
        for (int i = 2; i < 8; i++) { if (g_kbd_report[i]) { cur = g_kbd_report[i]; break; } }
        if (cur == prev_keycode) continue;
        prev_keycode = cur;
        if (!cur) continue;
        char c = hid_report_to_char(g_kbd_report);
        if (c) return c;
    }
}

void usb_hid_kbd_readline(char *buf, int maxlen) {
    if (!g_kbd_ready) { buf[0] = '\0'; return; }
    int i = 0;
    while (i < maxlen - 1) {
        char c = usb_hid_kbd_getchar();
        if (!c) continue;
        if (c == '\n') { terminal_putchar('\n'); break; }
        if (c == '\b') { if (i > 0) { i--; terminal_putchar('\b'); } continue; }
        buf[i++] = c;
        terminal_putchar(c);
    }
    buf[i] = '\0';
}
