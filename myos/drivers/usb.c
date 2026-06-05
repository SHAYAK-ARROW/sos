/*
 * usb.c — xHCI USB 3.0 Mass Storage Driver with Partition Table Debugging
 *
 * Handles:
 *   A. BIOS handoff (LEGSUP)
 *   B. 32-bit BAR0, 64-bit BAR0 (low 32 bits only, high must be 0)
 *   C. Full USB enumeration: GET DESCRIPTOR → parse EPs → Configure EP
 *   D. SET CONFIGURATION before Configure EP
 *   E. CErr=3, cycle bit tracking, Link TRB
 *   F. Partition table: MBR and GPT, dynamic storage partition selection
 *   G. g_ctx_dwords exported for usb_hid_kbd.c
 *   H. Partition table debugging output
 */

#include "usb.h"
#include "../mm/pmm.h"
#include <stdint.h>

extern void terminal_writeline(const char *s);
extern void terminal_write(const char *s);

/* ── Serial ───────────────────────────────────────────────────────────── */
static inline void outb(uint16_t p, uint8_t v)
    { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p)
    { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outl(uint16_t p, uint32_t v)
    { __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint32_t inl(uint16_t p)
    { uint32_t v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }

static void serial_init(void) {
    outb(0x3F8+1, 0x00); outb(0x3F8+3, 0x80); outb(0x3F8+0, 0x03);
    outb(0x3F8+1, 0x00); outb(0x3F8+3, 0x03); outb(0x3F8+2, 0xC7);
    outb(0x3F8+4, 0x0B);
}
static void serial_putc(char c) {
    for (uint32_t t = 0; t < 50000u; t++) {
        if (inb(0x3FD) & 0x20) { outb(0x3F8, (uint8_t)c); return; }
    }
}
static void serial_write(const char *s) {
    const char *p = s;
    while (*p) serial_putc(*p++);
    {
        int len = 0; while (s[len]) len++;
        if (len > 0 && s[len-1] == '\n') {
            char buf[128]; int i = 0;
            while (s[i] && s[i] != '\n' && i < 126) { buf[i] = s[i]; i++; }
            buf[i] = '\0';
            terminal_writeline(buf);
        }
    }
}
static void serial_write_hex(uint32_t v) {
    char b[11] = "0x00000000";
    const char *h = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) { b[i+2] = h[v & 0xF]; v >>= 4; }
    serial_write(b);
}
static void serial_write_dec(uint32_t v) {
    char b[12]; int i = 10; b[11] = 0;
    if (!v) { serial_write("0"); return; }
    while (v && i >= 0) { b[i--] = '0' + (v % 10); v /= 10; }
    serial_write(b + i + 1);
}

/* ── PCI ──────────────────────────────────────────────────────────────── */
#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC
static uint32_t pci_read(uint8_t b, uint8_t s, uint8_t f, uint8_t o) {
    outl(PCI_ADDR, 0x80000000u | ((uint32_t)b<<16) | ((uint32_t)s<<11)
                                | ((uint32_t)f<<8)  | (o & 0xFC));
    return inl(PCI_DATA);
}
static void pci_write(uint8_t b, uint8_t s, uint8_t f, uint8_t o, uint32_t v) {
    outl(PCI_ADDR, 0x80000000u | ((uint32_t)b<<16) | ((uint32_t)s<<11)
                                | ((uint32_t)f<<8)  | (o & 0xFC));
    outl(PCI_DATA, v);
}
#define mmio_r32(a)    (*(volatile uint32_t *)(uintptr_t)(a))
#define mmio_w32(a, v) (*(volatile uint32_t *)(uintptr_t)(a) = (v))

/* ── TRB type constants ──────────────────────────────────────────────── */
#define TRB_SETUP          2
#define TRB_DATA           3
#define TRB_STATUS         4
#define TRB_NORMAL         1
#define TRB_ENABLE_SLOT    9
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIGURE_EP   12
#define TRB_LINK           6
#define TRB_TRANSFER_EVT   32
#define TRB_CMD_COMPLETION 33
#define CC_SUCCESS         1
#define CC_SHORT_PACKET    13

/* ── Exported globals ────────────────────────────────────────────────── */
uint32_t g_xhci_op_base  = 0;
uint32_t g_xhci_rt_base  = 0;
uint32_t g_xhci_db_base  = 0;
uint32_t g_xhci_num_ports = 0;
uint32_t g_ctx_dwords    = 8;   /* updated from HCCPARAMS1[CSZ] in usb_init */

xhci_trb_t  *g_cmd_ring = 0;
xhci_trb_t  *g_evt_ring = 0;
xhci_erst_t *g_erst     = 0;
uint64_t    *g_dcbaa    = 0;

uint32_t g_cmd_idx   = 0, g_cmd_cycle  = 1;
uint32_t g_evt_idx   = 0, g_evt_cycle  = 1;

uint8_t g_xhci_occupied_ports[XHCI_MAX_PORTS] = {0};

/* ── Private ──────────────────────────────────────────────────────────── */
static xhci_trb_t *bulk_in_ring  = 0;
static xhci_trb_t *bulk_out_ring = 0;
static xhci_trb_t *ep0_ring      = 0;

static uint32_t *device_context  = 0;
static uint32_t *input_context   = 0;
static uint32_t *input_context2  = 0;
static uint64_t *scratchpad_buf  = 0;
static uint64_t *scratch_ptr     = 0;
static uint8_t  *desc_buf        = 0;

static uint32_t pen_slot_id  = 0;
static uint32_t g_ep_out_dci = 2;
static uint32_t g_ep_in_dci  = 3;
static uint32_t g_bulk_maxpkt = 512;
static uint32_t msc_in_idx   = 0, msc_out_idx  = 0;
static uint32_t msc_in_cycle = 1, msc_out_cycle = 1;
static uint32_t ep0_idx      = 0, ep0_cycle     = 1;

static int      g_disk_ready = 0;
static uint32_t g_part2_lba  = 0;
static uint32_t g_part2_size = 0;
static uint32_t g_tag        = 1;

/* ── Helpers ──────────────────────────────────────────────────────────── */
static void memclr(void *p, uint32_t n) {
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) b[i] = 0;
}
static void io_delay(uint32_t us) {
    for (uint32_t i = 0; i < us; i++) outb(0x80, 0);
}
static void setup_transfer_ring(xhci_trb_t *ring, uint32_t cy) {
    ring[63].param_lo = (uint32_t)(uintptr_t)ring;
    ring[63].param_hi = 0;
    ring[63].status   = 0;
    ring[63].control  = (TRB_LINK << 10) | 0x02u | cy;
}

/* ── BIOS Handoff ─────────────────────────────────────────────────────── */
static void xhci_bios_handoff(uint32_t bar0) {
    uint32_t hccparams1 = mmio_r32(bar0 + 0x10);
    uint32_t xecp = (hccparams1 >> 16) & 0xFFFF;
    if (xecp == 0) { serial_write("No xECP, skip BIOS handoff\n"); return; }

    uint32_t cap_off = xecp * 4;
    int found = 0;
    for (int i = 0; i < 32 && cap_off != 0; i++) {
        uint32_t cap    = mmio_r32(bar0 + cap_off);
        uint8_t  cap_id = (uint8_t)(cap & 0xFF);
        uint32_t next   = ((cap >> 8) & 0xFF) * 4;

        if (cap_id == 1) {
            found = 1;
            serial_write("LEGSUP found, requesting OS ownership\n");
            uint32_t legsup = mmio_r32(bar0 + cap_off);
            mmio_w32(bar0 + cap_off, legsup | (1u << 24));
            for (int t = 0; t < 5000; t++) {
                legsup = mmio_r32(bar0 + cap_off);
                if (!(legsup & (1u << 16))) break;
                io_delay(1000);
            }
            legsup = mmio_r32(bar0 + cap_off);
            if (legsup & (1u << 16))
                serial_write("WARNING: BIOS did not release xHCI!\n");
            else
                serial_write("BIOS handoff OK\n");
            uint32_t ctlsts = mmio_r32(bar0 + cap_off + 4);
            ctlsts &= ~0x000E1FFFu;
            mmio_w32(bar0 + cap_off + 4, ctlsts);
            break;
        }
        if (next == 0) break;
        cap_off += next;
    }
    if (!found) serial_write("No LEGSUP cap found\n");
}

/* ── xHCI events & commands ───────────────────────────────────────────── */
void xhci_ring_doorbell(uint32_t slot, uint32_t target) {
    mmio_w32(g_xhci_db_base + slot * 4, target);
}

int xhci_wait_event(uint32_t want_type, uint32_t timeout_ms) {
    uint32_t limit = timeout_ms * 1000, waited = 0;
    while (waited < limit) {
        volatile xhci_trb_t *evt = &g_evt_ring[g_evt_idx];
        if ((evt->control & 0x01) == g_evt_cycle) {
            uint32_t type = (evt->control >> 10) & 0x3F;
            uint32_t code = (evt->status  >> 24) & 0xFF;
            uint32_t sid  = (evt->control >> 24) & 0xFF;
            g_evt_idx++;
            if (g_evt_idx >= 64) { g_evt_idx = 0; g_evt_cycle ^= 1; }
            uint32_t erdp = (uint32_t)(uintptr_t)&g_evt_ring[g_evt_idx];
            mmio_w32(g_xhci_rt_base + 0x20 + 0x18, erdp | 0x08);
            mmio_w32(g_xhci_rt_base + 0x20 + 0x1C, 0);
            if (type == want_type) {
                if (want_type == TRB_CMD_COMPLETION) {
                    if (code != CC_SUCCESS) {
                        serial_write(" CMD FAIL c="); serial_write_dec(code);
                        serial_write("\n"); return -1;
                    }
                    return (int)sid;
                }
                return (code == CC_SUCCESS || code == CC_SHORT_PACKET) ? 0 : -1;
            }
            waited += 100;
        }
        io_delay(1); waited++;
    }
    return -1;
}

int xhci_send_cmd(uint32_t type, uint32_t p0, uint32_t p1,
                   uint32_t st, uint32_t slot) {
    uint32_t idx = g_cmd_idx;
    g_cmd_ring[idx].param_lo = p0;
    g_cmd_ring[idx].param_hi = p1;
    g_cmd_ring[idx].status   = st;
    g_cmd_ring[idx].control  = (type << 10) | (slot << 24) | g_cmd_cycle;
    g_cmd_idx++;
    if (g_cmd_idx >= 63) {
        g_cmd_ring[63].param_lo = (uint32_t)(uintptr_t)g_cmd_ring;
        g_cmd_ring[63].param_hi = 0;
        g_cmd_ring[63].status   = 0;
        g_cmd_ring[63].control  = (TRB_LINK << 10) | 0x02u | g_cmd_cycle;
        g_cmd_idx = 0; g_cmd_cycle ^= 1;
    }
    serial_write(" CMD t="); serial_write_dec(type);
    serial_write(" slot="); serial_write_dec(slot); serial_write("\n");
    xhci_ring_doorbell(0, 0);
    return xhci_wait_event(TRB_CMD_COMPLETION, 2000);
}

/* ── EP0 Control Transfer (shared, used by mass storage slot) ──────────── */
static void ep0_enqueue(uint32_t p0, uint32_t p1, uint32_t st, uint32_t ctrl) {
    ctrl = (ctrl & ~1u) | (ep0_cycle & 1u);
    ep0_ring[ep0_idx].param_lo = p0;
    ep0_ring[ep0_idx].param_hi = p1;
    ep0_ring[ep0_idx].status   = st;
    ep0_ring[ep0_idx].control  = ctrl;
    ep0_idx++;
    if (ep0_idx >= 63) {
        ep0_ring[63].control = (TRB_LINK << 10) | 0x02u | (ep0_cycle & 1u);
        ep0_idx = 0; ep0_cycle ^= 1;
    }
}

/*
 * xhci_control_transfer — standards-compliant EP0 control transfer.
 * TRT: 0 = no-data, 2 = OUT data, 3 = IN data  (xHCI spec §6.4.1.2.1)
 */
int xhci_control_transfer(uint8_t bmRT, uint8_t bReq, uint16_t wVal,
                           uint16_t wIdx, uint16_t wLen, void *buf, int is_in) {
    uint32_t setup_lo = (uint32_t)bmRT | ((uint32_t)bReq << 8)
                      | ((uint32_t)wVal << 16);
    uint32_t setup_hi = (uint32_t)wIdx | ((uint32_t)wLen << 16);
    uint32_t trt = (wLen == 0) ? 0u : (is_in ? 3u : 2u);
    ep0_enqueue(setup_lo, setup_hi, 8u,
                (TRB_SETUP << 10) | (1u << 6) | (trt << 16));
    if (wLen > 0 && buf) {
        uint32_t dir = is_in ? (1u << 16) : 0u;
        ep0_enqueue((uint32_t)(uintptr_t)buf, 0, (uint32_t)wLen,
                    (TRB_DATA << 10) | (1u << 5) | dir);
    }
    /* Status stage direction is opposite to data stage;
     * for no-data transfers the status stage is always IN. */
    uint32_t sdir = (wLen == 0 || !is_in) ? (1u << 16) : 0u;
    ep0_enqueue(0, 0, 0, (TRB_STATUS << 10) | (1u << 5) | sdir);
    xhci_ring_doorbell(pen_slot_id, 1);
    if (wLen > 0 && buf) {
        if (xhci_wait_event(TRB_TRANSFER_EVT, 2000) < 0) return -1;
    }
    if (xhci_wait_event(TRB_TRANSFER_EVT, 2000) < 0) return -1;
    return 0;
}

/* ── Bulk Transfer ────────────────────────────────────────────────────── */
static int xhci_bulk_transfer(void *data, uint32_t size,
                               int is_in, uint32_t timeout_ms) {
    xhci_trb_t *ring  = is_in ? bulk_in_ring   : bulk_out_ring;
    uint32_t   *idx   = is_in ? &msc_in_idx    : &msc_out_idx;
    uint32_t   *cycle = is_in ? &msc_in_cycle  : &msc_out_cycle;
    uint32_t    db    = is_in ? g_ep_in_dci    : g_ep_out_dci;
    if (*idx >= 63) *idx = 0;
    ring[*idx].param_lo = (uint32_t)(uintptr_t)data;
    ring[*idx].param_hi = 0;
    ring[*idx].status   = size;
    ring[*idx].control  = (TRB_NORMAL << 10) | (1u << 5) | (*cycle & 1u);
    (*idx)++;
    if (*idx >= 63) {
        ring[63].control = (TRB_LINK << 10) | 0x02u | (*cycle & 1u);
        *idx = 0; *cycle ^= 1u;
    }
    xhci_ring_doorbell(pen_slot_id, db);
    return xhci_wait_event(TRB_TRANSFER_EVT, timeout_ms);
}

/* ── BOT Mass Storage ─────────────────────────────────────────────────── */
typedef struct {
    uint32_t sig, tag, dlen;
    uint8_t  flags, lun, cblen, cb[16];
} __attribute__((packed)) cbw_t;

typedef struct {
    uint32_t sig, tag, residue;
    uint8_t  status;
} __attribute__((packed)) csw_t;

static cbw_t   dma_cbw __attribute__((aligned(64)));
static csw_t   dma_csw __attribute__((aligned(64)));
static uint8_t dma_mbr[512] __attribute__((aligned(64)));

static int bot_scsi(uint8_t op, uint32_t lba, uint8_t cnt,
                    uint8_t *buf, int is_in, uint32_t timeout_ms) {
    if (!g_disk_ready) return -1;
    memclr(&dma_cbw, sizeof(cbw_t));
    dma_cbw.sig   = 0x43425355u;
    dma_cbw.tag   = g_tag++;
    dma_cbw.dlen  = (uint32_t)cnt * 512;
    dma_cbw.flags = is_in ? 0x80u : 0x00u;
    dma_cbw.cblen = 10;
    dma_cbw.cb[0] = op;
    dma_cbw.cb[2] = (uint8_t)(lba >> 24);
    dma_cbw.cb[3] = (uint8_t)(lba >> 16);
    dma_cbw.cb[4] = (uint8_t)(lba >>  8);
    dma_cbw.cb[5] = (uint8_t) lba;
    dma_cbw.cb[8] = cnt;
    if (xhci_bulk_transfer(&dma_cbw, 31, 0, timeout_ms) < 0) return -1;
    if (xhci_bulk_transfer(buf, dma_cbw.dlen, is_in, timeout_ms) < 0) return -1;
    if (xhci_bulk_transfer(&dma_csw, 13, 1, timeout_ms) < 0) return -1;
    return (dma_csw.status == 0) ? 0 : -1;
}

/* ── Partition table parsing ──────────────────────────────────────────── */
/*
 * MBR partition entry at byte offset 0x1BE + n*16:
 *   +0  status (0x80 = bootable)
 *   +1  CHS first
 *   +4  type
 *   +8  LBA start   (uint32_t, little-endian)
 *   +12 LBA length  (uint32_t, little-endian)
 *
 * GPT protective MBR has type 0xEE in the first entry.
 * GPT header is at LBA 1.
 */

/* GPT header (first 92 bytes of LBA 1) */
typedef struct {
    uint8_t  signature[8];    /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alt_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entries_lba;
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;
    uint32_t partition_array_crc32;
} __attribute__((packed)) gpt_header_t;

/* GPT partition entry (128 bytes) */
typedef struct {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t start_lba;
    uint64_t end_lba;
    uint64_t attributes;
    uint16_t name[36];   /* UTF-16LE, unused here */
} __attribute__((packed)) gpt_entry_t;

/* All-zero GUID means unused entry */
static int guid_is_zero(const uint8_t *g) {
    for (int i = 0; i < 16; i++) if (g[i]) return 0;
    return 1;
}

static uint8_t gpt_sector[512] __attribute__((aligned(64)));

/* Debug partition table information */
static void debug_partition_info(uint8_t *mbr) {
    serial_write("=== MBR Partition Debug ===\n");
    for (int n = 0; n < 4; n++) {
        uint8_t *pe = &mbr[0x1BE + n * 16];
        uint8_t status = pe[0];
        uint8_t type = pe[4];
        uint32_t lba = *(uint32_t *)(pe + 8);
        uint32_t size = *(uint32_t *)(pe + 12);
        
        serial_write("Partition ");
        serial_write_dec(n + 1);
        serial_write(": Status=");
        serial_write_hex(status);
        serial_write(" Type=");
        serial_write_hex(type);
        serial_write(" LBA=");
        serial_write_dec(lba);
        serial_write(" Size=");
        serial_write_dec(size);
        serial_write("\n");
    }
}

/*
 * usb_parse_partition_table
 *
 * Reads LBA 0 (MBR) and determines whether the drive uses MBR or GPT.
 * For MBR: scans all four primary entries, skips the boot partition
 *          (type 0xEF = EFI System, 0x0B/0x0C = FAT32, 0x04/0x06/0x0E = FAT16,
 *          or any entry whose LBA overlaps with LBA 0..2047 where GRUB lives),
 *          selects the first non-boot partition with a non-zero size.
 * For GPT: reads the partition entry array, skips the EFI System partition
 *          (type GUID starting with 28 73 2A C1 ...) and the Microsoft
 *          Basic Data partition that contains the bootloader files,
 *          selects the first remaining data partition.
 *
 * Sets g_part2_lba and g_part2_size on success.
 * On failure leaves both at 0 — fat16 will format a fresh filesystem
 * at LBA offset 0 (which will fail gracefully if the disk is read-only).
 */
static void usb_parse_partition_table(void) {
    g_part2_lba  = 0;
    g_part2_size = 0;
    g_disk_ready = 1;

    terminal_writeline("USB: Reading partition table...");
    io_delay(300000);

    /* Read MBR (LBA 0) */
    if (bot_scsi(0x28, 0, 1, dma_mbr, 1, 1500) != 0) {
        terminal_writeline("USB: MBR read failed — no storage partition.");
        return;
    }
    if (dma_mbr[510] != 0x55 || dma_mbr[511] != 0xAA) {
        terminal_writeline("USB: No MBR signature — no storage partition.");
        return;
    }

    /* Print debug info for all partitions */
    debug_partition_info(dma_mbr);

    /* Check for GPT protective MBR: first partition type == 0xEE */
    uint8_t first_type = dma_mbr[0x1BE + 4];
    if (first_type == 0xEE) {
        /* GPT disk — read LBA 1 for GPT header */
        serial_write("GPT disk detected\n");
        if (bot_scsi(0x28, 1, 1, gpt_sector, 1, 1500) != 0) {
            terminal_writeline("USB: GPT header read failed.");
            return;
        }
        gpt_header_t *gh = (gpt_header_t *)gpt_sector;
        /* Verify signature "EFI PART" */
        const uint8_t efi_sig[8] = {0x45,0x46,0x49,0x20,0x50,0x41,0x52,0x54};
        for (int i = 0; i < 8; i++) {
            if (gh->signature[i] != efi_sig[i]) {
                terminal_writeline("USB: GPT signature invalid.");
                return;
            }
        }

        uint32_t entry_lba  = (uint32_t)gh->partition_entries_lba;
        uint32_t num_ent    = gh->num_partition_entries;
        uint32_t entry_size = gh->partition_entry_size;
        if (entry_size < 128 || entry_size > 512 || num_ent == 0) {
            terminal_writeline("USB: GPT entry size invalid.");
            return;
        }

        /* EFI System Partition GUID (mixed-endian as stored on disk):
         * {28732AC1-1FF8-D211-BA4B-00A0C93EC93B}
         * Bytes: C1 2A 73 28 F8 1F 11 D2 BA 4B 00 A0 C9 3E C9 3B */
        static const uint8_t efi_system_guid[16] = {
            0xC1,0x2A,0x73,0x28, 0xF8,0x1F, 0x11,0xD2,
            0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B
        };

        /* Walk partition entries — up to 128, one 512-byte sector at a time */
        uint32_t boot_max_lba = 0; /* highest LBA used by first non-ESP partition */
        uint32_t found_lba = 0, found_size = 0;
        uint32_t entries_per_sector = 512 / entry_size;
        uint32_t sector_count = (num_ent + entries_per_sector - 1) / entries_per_sector;

        /* First pass: find the end of the boot/EFI partitions */
        for (uint32_t s = 0; s < sector_count && s < 8; s++) {
            if (bot_scsi(0x28, entry_lba + s, 1, gpt_sector, 1, 1500) != 0) break;
            for (uint32_t e = 0; e < entries_per_sector; e++) {
                uint32_t off = e * entry_size;
                if (off + entry_size > 512) break;
                gpt_entry_t *ent = (gpt_entry_t *)(gpt_sector + off);
                if (guid_is_zero(ent->type_guid)) continue;
                /* EFI System Partition — this is the boot partition */
                int is_efi = 1;
                for (int i = 0; i < 16; i++)
                    if (ent->type_guid[i] != efi_system_guid[i]) { is_efi = 0; break; }
                if (is_efi) {
                    uint32_t end = (uint32_t)ent->end_lba;
                    if (end > boot_max_lba) boot_max_lba = end;
                }
            }
        }

        /* Second pass: find first data partition that starts after boot area */
        for (uint32_t s = 0; s < sector_count && s < 8; s++) {
            if (bot_scsi(0x28, entry_lba + s, 1, gpt_sector, 1, 1500) != 0) break;
            for (uint32_t e = 0; e < entries_per_sector; e++) {
                uint32_t off = e * entry_size;
                if (off + entry_size > 512) break;
                gpt_entry_t *ent = (gpt_entry_t *)(gpt_sector + off);
                if (guid_is_zero(ent->type_guid)) continue;
                int is_efi = 1;
                for (int i = 0; i < 16; i++)
                    if (ent->type_guid[i] != efi_system_guid[i]) { is_efi = 0; break; }
                if (is_efi) continue; /* skip boot partition */
                uint32_t start = (uint32_t)ent->start_lba;
                uint32_t end   = (uint32_t)ent->end_lba;
                if (start > boot_max_lba && end >= start) {
                    found_lba  = start;
                    found_size = end - start + 1;
                    goto gpt_done;
                }
            }
        }
gpt_done:
        if (found_lba != 0) {
            g_part2_lba  = found_lba;
            g_part2_size = found_size;
            serial_write("GPT storage partition: LBA=");
            serial_write_dec(g_part2_lba);
            serial_write(" size="); serial_write_dec(g_part2_size);
            serial_write("\n");
            terminal_writeline("USB: GPT storage partition found.");
        } else {
            terminal_writeline("USB: No GPT storage partition found.");
        }
        return;
    }

    /* MBR disk — scan all four primary partition entries */
    /* The boot partition is whichever entry is marked active (0x80) OR
     * contains a known bootloader filesystem type and starts at a low LBA.
     * Strategy: collect all valid entries, skip active ones, pick the first
     * non-active entry with a non-zero size. */
    uint32_t found_lba = 0, found_size = 0;

    for (int n = 0; n < 4; n++) {
        uint8_t *pe     = &dma_mbr[0x1BE + n * 16];
        uint8_t  status = pe[0];
        uint8_t  type   = pe[4];
        uint32_t lba    = *(uint32_t *)(pe + 8);
        uint32_t size   = *(uint32_t *)(pe + 12);

        if (type == 0x00) continue;  /* empty entry */
        if (size == 0)    continue;  /* zero-length entry */

        /* Skip the active (bootable) partition */
        if (status == 0x80) {
            serial_write("MBR P"); serial_write_dec((uint32_t)n + 1);
            serial_write(" = boot (active), LBA="); serial_write_dec(lba);
            serial_write("\n");
            continue;
        }

        /* Skip extended partition containers */
        if (type == 0x05 || type == 0x0F || type == 0x85) continue;

        if (found_lba == 0) {
            found_lba  = lba;
            found_size = size;
            serial_write("MBR P"); serial_write_dec((uint32_t)n + 1);
            serial_write(" = storage, LBA="); serial_write_dec(lba);
            serial_write(" size="); serial_write_dec(size);
            serial_write("\n");
        }
    }

    if (found_lba != 0) {
        g_part2_lba  = found_lba;
        g_part2_size = found_size;
        terminal_writeline("USB: MBR storage partition found.");
    } else {
        terminal_writeline("USB: No non-boot MBR partition found — no storage.");
    }
}

int  usb_read_sector(uint32_t lba, uint8_t *buf)
    { return bot_scsi(0x28, lba + g_part2_lba, 1, buf, 1, 10000); }
int  usb_write_sector(uint32_t lba, uint8_t *buf)
    { return bot_scsi(0x2A, lba + g_part2_lba, 1, buf, 0, 10000); }
int  usb_disk_present(void)  { return g_disk_ready; }
uint32_t usb_part2_lba(void)  { return g_part2_lba; }
uint32_t usb_part2_size(void) { return g_part2_size; }

/* ── Main Init ───────────────────────────────────────────────────────── */
int usb_init(void) {
    static int s_init_done = 0;
    if (s_init_done) {
        serial_write("usb_init: already done, skipping\n");
        return g_disk_ready ? 0 : -1;
    }
    s_init_done = 1;

    serial_init();
    g_disk_ready = 0;
    serial_write("=== usb_init ===\n");

    for (int bus = 0; bus < 256; bus++) for (int dev = 0; dev < 32; dev++) {
        uint32_t r0 = pci_read((uint8_t)bus, (uint8_t)dev, 0, 0);
        if ((r0 & 0xFFFF) == 0xFFFF || (r0 & 0xFFFF) == 0) continue;
        uint32_t r8 = pci_read((uint8_t)bus, (uint8_t)dev, 0, 8);
        if (((r8>>24)&0xFF) != 0x0C || ((r8>>16)&0xFF) != 0x03
                                    || ((r8>>8) &0xFF) != 0x30) continue;

        serial_write("xHCI @ "); serial_write_dec((uint32_t)bus);
        serial_write(":"); serial_write_dec((uint32_t)dev); serial_write("\n");
        terminal_writeline("USB: xHCI found!");

        pci_write((uint8_t)bus, (uint8_t)dev, 0, 0x04,
                  pci_read((uint8_t)bus, (uint8_t)dev, 0, 0x04) | 0x06);

        uint32_t bar0 = pci_read((uint8_t)bus, (uint8_t)dev, 0, 0x10);
        if ((bar0 & 0x06) == 0x04) {
            uint32_t hi = pci_read((uint8_t)bus, (uint8_t)dev, 0, 0x14);
            if (hi != 0) { serial_write("BAR0 >4GB skip\n"); continue; }
        }
        bar0 &= ~0xFu;
        if (bar0 == 0) { serial_write("BAR0=0 skip\n"); continue; }
        serial_write("BAR0="); serial_write_hex(bar0); serial_write("\n");

        uint8_t  cap_len    = (uint8_t)(mmio_r32(bar0) & 0xFF);
        uint32_t hcs2       = mmio_r32(bar0 + 0x08);
        uint32_t hccparams1 = mmio_r32(bar0 + 0x10);
        g_xhci_op_base  = bar0 + cap_len;
        g_xhci_rt_base  = bar0 + (mmio_r32(bar0 + 0x18) & ~0x1Fu);
        g_xhci_db_base  = bar0 + (mmio_r32(bar0 + 0x14) & ~0x03u);
        g_xhci_num_ports = (mmio_r32(bar0 + 0x04) >> 24) & 0xFF;
        uint32_t sp_count = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 16) & 0x1F);
        g_ctx_dwords = (hccparams1 & (1u << 2)) ? 16u : 8u;

        serial_write("cap="); serial_write_dec(cap_len);
        serial_write(" ports="); serial_write_dec(g_xhci_num_ports);
        serial_write(" ctx_dw="); serial_write_dec(g_ctx_dwords);
        serial_write("\n");

        terminal_writeline("USB: BIOS handoff...");
        xhci_bios_handoff(bar0);

        terminal_writeline("USB: Stopping...");
        mmio_w32(g_xhci_op_base, mmio_r32(g_xhci_op_base) & ~0x01u);
        for (int t = 0; t < 500000; t++) {
            if (mmio_r32(g_xhci_op_base + 0x04) & 0x01u) break;
            io_delay(10);
        }

        terminal_writeline("USB: Resetting...");
        mmio_w32(g_xhci_op_base, mmio_r32(g_xhci_op_base) | 0x02u);
        for (int t = 0; t < 500000; t++) {
            if (!(mmio_r32(g_xhci_op_base) & 0x02u)) break;
            io_delay(10);
        }
        io_delay(500000);
        if (mmio_r32(g_xhci_op_base) & 0x02u) {
            serial_write("Reset hung!\n"); return -1;
        }

        terminal_writeline("USB: Allocating...");
        g_cmd_ring     = (xhci_trb_t *)(uintptr_t)pmm_alloc_page();
        g_evt_ring     = (xhci_trb_t *)(uintptr_t)pmm_alloc_page();
        g_erst         = (xhci_erst_t*)(uintptr_t)pmm_alloc_page();
        g_dcbaa        = (uint64_t    *)(uintptr_t)pmm_alloc_page();
        bulk_in_ring   = (xhci_trb_t *)(uintptr_t)pmm_alloc_page();
        bulk_out_ring  = (xhci_trb_t *)(uintptr_t)pmm_alloc_page();
        ep0_ring       = (xhci_trb_t *)(uintptr_t)pmm_alloc_page();
        device_context = (uint32_t    *)(uintptr_t)pmm_alloc_page();
        input_context  = (uint32_t    *)(uintptr_t)pmm_alloc_page();
        input_context2 = (uint32_t    *)(uintptr_t)pmm_alloc_page();
        scratchpad_buf = (uint64_t    *)(uintptr_t)pmm_alloc_page();
        scratch_ptr    = (uint64_t    *)(uintptr_t)pmm_alloc_page();
        desc_buf       = (uint8_t     *)(uintptr_t)pmm_alloc_page();

        if (!g_cmd_ring || !g_evt_ring || !g_erst || !g_dcbaa ||
            !bulk_in_ring || !bulk_out_ring || !ep0_ring ||
            !device_context || !input_context || !input_context2 ||
            !scratchpad_buf || !scratch_ptr || !desc_buf) {
            serial_write("PMM OOM\n"); return -1;
        }
        memclr(g_cmd_ring,    4096); memclr(g_evt_ring,     4096);
        memclr(g_erst,        4096); memclr(g_dcbaa,         4096);
        memclr(bulk_in_ring,  4096); memclr(bulk_out_ring,   4096);
        memclr(ep0_ring,      4096); memclr(device_context,  4096);
        memclr(input_context, 4096); memclr(input_context2,  4096);
        memclr(scratchpad_buf,4096); memclr(scratch_ptr,      4096);
        memclr(desc_buf,      4096);

        g_cmd_idx = 0; g_cmd_cycle  = 1;
        g_evt_idx = 0; g_evt_cycle  = 1;
        ep0_idx   = 0; ep0_cycle    = 1;
        msc_in_idx  = 0; msc_out_idx  = 0;
        msc_in_cycle = 1; msc_out_cycle = 1;
        g_ep_out_dci = 2; g_ep_in_dci = 3; g_bulk_maxpkt = 512;

        setup_transfer_ring(bulk_in_ring,  1);
        setup_transfer_ring(bulk_out_ring, 1);
        setup_transfer_ring(ep0_ring,      1);

        if (sp_count > 0) {
            scratch_ptr[0] = (uint64_t)(uintptr_t)scratchpad_buf;
            g_dcbaa[0] = (uint64_t)(uintptr_t)scratch_ptr;
        } else {
            g_dcbaa[0] = 0;
        }

        mmio_w32(g_xhci_op_base + 0x30, (uint32_t)(uintptr_t)g_dcbaa);
        mmio_w32(g_xhci_op_base + 0x34, 0);
        mmio_w32(g_xhci_op_base + 0x18, (uint32_t)(uintptr_t)g_cmd_ring | 1u);
        mmio_w32(g_xhci_op_base + 0x1C, 0);
        mmio_w32(g_xhci_op_base + 0x38, (mmio_r32(g_xhci_op_base + 0x38) & ~0xFFu) | 1u);

        uint32_t ir0 = g_xhci_rt_base + 0x20;
        g_erst->addr_lo = (uint32_t)(uintptr_t)g_evt_ring;
        g_erst->addr_hi = 0; g_erst->size = 64; g_erst->rsvd = 0;
        mmio_w32(ir0 + 0x08, 1);
        mmio_w32(ir0 + 0x10, (uint32_t)(uintptr_t)g_erst);
        mmio_w32(ir0 + 0x14, 0);
        mmio_w32(ir0 + 0x18, (uint32_t)(uintptr_t)g_evt_ring | 0x08u);
        mmio_w32(ir0 + 0x1C, 0);

        terminal_writeline("USB: Starting...");
        mmio_w32(g_xhci_op_base, 0x01u);
        for (int t = 0; t < 500000; t++) {
            if (!(mmio_r32(g_xhci_op_base + 0x04) & 0x01u)) break;
            io_delay(10);
        }
        if (mmio_r32(g_xhci_op_base + 0x04) & 0x01u) {
            serial_write("Start failed!\n"); return -1;
        }

        /* Port scan */
        terminal_writeline("USB: Port scan...");
        io_delay(500000);  /* Increased from 200000 to 500ms for better port settling */
        int connected_port = -1;
        for (uint32_t p = 0; p < g_xhci_num_ports; p++) {
            uint32_t pr = g_xhci_op_base + 0x400 + p * 0x10;
            uint32_t ps = mmio_r32(pr);
            serial_write("Port "); serial_write_dec(p);
            serial_write(" st="); serial_write_hex(ps); serial_write("\n");
            if ((ps & 0x01u) && connected_port < 0) {
                connected_port = (int)p;
                mmio_w32(pr, (ps & ~0x00020000u) | (1u << 4));
                io_delay(50000);
                for (int t = 0; t < 2000; t++) {
                    if (!(mmio_r32(pr) & (1u << 4))) break;
                    io_delay(1000);
                }
                io_delay(100000);
                serial_write("Port "); serial_write_dec(p);
                serial_write(" reset="); serial_write_hex(mmio_r32(pr));
                serial_write("\n");
            }
        }
        if (connected_port < 0) {
            terminal_writeline("USB: No device!"); return -1;
        }
        io_delay(200000);

        /* Port speed → EP0 max packet size */
        uint32_t pst = mmio_r32(g_xhci_op_base + 0x400
                                + (uint32_t)connected_port * 0x10);
        uint32_t port_speed = (pst >> 10) & 0x0Fu;
        if (port_speed == 0) port_speed = 3;
        uint32_t maxpkt0;
        if      (port_speed >= 4) maxpkt0 = 512u; /* SuperSpeed */
        else if (port_speed == 3) maxpkt0 = 64u;  /* HighSpeed  */
        else                      maxpkt0 = 8u;   /* Full/Low   */

        serial_write("speed="); serial_write_dec(port_speed);
        serial_write(" maxpkt0="); serial_write_dec(maxpkt0); serial_write("\n");

        /* Enable Slot */
        terminal_writeline("USB: Enable Slot...");
        int slot = xhci_send_cmd(TRB_ENABLE_SLOT, 0, 0, 0, 0);
        if (slot <= 0 || slot > 255) {
            serial_write("Enable Slot FAIL\n"); return -1;
        }
        pen_slot_id = (uint32_t)slot;

        uint32_t *new_dev_ctx = (uint32_t *)(uintptr_t)pmm_alloc_page();
        memclr(new_dev_ctx, 4096);
        g_dcbaa[pen_slot_id] = (uint64_t)(uintptr_t)new_dev_ctx;
        memclr(input_context, 4096);

        serial_write("slot="); serial_write_dec(pen_slot_id); serial_write("\n");

        /* Address Device */
        input_context[0] = 0;
        input_context[1] = 0x03u;
        uint32_t *sc = &input_context[g_ctx_dwords];
        sc[0] = (port_speed << 20) | (1u << 27);
        sc[1] = ((uint32_t)(connected_port + 1) << 16);
        sc[2] = 0; sc[3] = 0;
        uint32_t *e0 = &input_context[g_ctx_dwords * 2];
        e0[0] = 3u;
        e0[1] = (4u << 3) | (maxpkt0 << 16);
        e0[2] = (uint32_t)(uintptr_t)ep0_ring | 1u;
        e0[3] = 0;

        terminal_writeline("USB: Address Device...");
        if (xhci_send_cmd(TRB_ADDRESS_DEVICE,
                          (uint32_t)(uintptr_t)input_context, 0, 0,
                          pen_slot_id) < 0) {
            serial_write("Address Device FAIL\n"); return -1;
        }
        serial_write("Address Device OK\n");

        /* GET Device Descriptor — update EP0 max packet from bMaxPacketSize0 */
        serial_write("GET DevDesc\n");
        memclr(desc_buf, 18);
        xhci_control_transfer(0x80, 6, 0x0100, 0, 18, desc_buf, 1);
        if (desc_buf[7] > 0 && port_speed < 4) {
            /* Update EP0 mps from descriptor (SuperSpeed always 512) */
            maxpkt0 = (uint32_t)desc_buf[7];
            serial_write("EP0 mps from descriptor=");
            serial_write_dec(maxpkt0); serial_write("\n");
        }

        /* GET Configuration Descriptor — parse Bulk EPs for mass storage */
        serial_write("GET CfgDesc\n");
        memclr(desc_buf, 255);
        xhci_control_transfer(0x80, 6, 0x0200, 0, 255, desc_buf, 1);
        uint32_t total = ((uint32_t)desc_buf[3] << 8) | desc_buf[2];
        if (total > 255) total = 255;
        serial_write("CfgDesc total="); serial_write_dec(total); serial_write("\n");

        uint32_t ci = 0;
        while (ci < total && desc_buf[ci] >= 2) {
            if (desc_buf[ci + 1] == 0x05) { /* Endpoint */
                uint8_t  ea   = desc_buf[ci + 2];
                uint8_t  attr = desc_buf[ci + 3];
                uint16_t mps  = ((uint16_t)desc_buf[ci + 5] << 8)
                              |            desc_buf[ci + 4];
                serial_write(" EP ea="); serial_write_hex(ea);
                serial_write(" attr="); serial_write_hex(attr);
                serial_write(" mps="); serial_write_dec(mps);
                serial_write("\n");
                if ((attr & 0x03) == 0x02) { /* Bulk */
                    uint8_t n = ea & 0x0F;
                    if (ea & 0x80) {
                        g_ep_in_dci  = (uint32_t)n * 2 + 1;
                        g_bulk_maxpkt = (uint32_t)mps;
                    } else {
                        g_ep_out_dci = (uint32_t)n * 2;
                    }
                }
            }
            ci += desc_buf[ci];
        }
        serial_write("ep_out_dci="); serial_write_dec(g_ep_out_dci);
        serial_write(" ep_in_dci="); serial_write_dec(g_ep_in_dci);
        serial_write(" mps="); serial_write_dec(g_bulk_maxpkt);
        serial_write("\n");

        /* SET CONFIGURATION — activates endpoints on the device side */
        serial_write("SET_CONFIG\n");
        xhci_control_transfer(0x00, 9, 0x0001, 0, 0, 0, 0);
        serial_write("SET_CONFIG done\n");
        io_delay(20000);

        /* Configure EP */
        memclr(input_context2, 4096);
        input_context2[0] = 0;
        uint32_t add_flags = 1u | (1u << g_ep_out_dci) | (1u << g_ep_in_dci);
        input_context2[1]  = add_flags;
        uint32_t ctx_ent = g_ep_in_dci > g_ep_out_dci ? g_ep_in_dci : g_ep_out_dci;
        uint32_t *sc2    = &input_context2[g_ctx_dwords];
        sc2[0] = (port_speed << 20) | (ctx_ent << 27);
        sc2[1] = ((uint32_t)(connected_port + 1) << 16);
        sc2[2] = 0; sc2[3] = 0;
        uint32_t *ep_out = &input_context2[g_ctx_dwords * (g_ep_out_dci + 1)];
        ep_out[0] = 3u;
        ep_out[1] = (2u << 3) | (g_bulk_maxpkt << 16);
        ep_out[2] = (uint32_t)(uintptr_t)bulk_out_ring | 1u;
        ep_out[3] = 0;
        uint32_t *ep_in  = &input_context2[g_ctx_dwords * (g_ep_in_dci  + 1)];
        ep_in[0] = 3u;
        ep_in[1] = (6u << 3) | (g_bulk_maxpkt << 16);
        ep_in[2] = (uint32_t)(uintptr_t)bulk_in_ring | 1u;
        ep_in[3] = 0;

        terminal_writeline("USB: Configure EP...");
        serial_write("add_flags="); serial_write_hex(add_flags); serial_write("\n");
        if (xhci_send_cmd(TRB_CONFIGURE_EP,
                          (uint32_t)(uintptr_t)input_context2, 0, 0,
                          pen_slot_id) < 0) {
            serial_write("Configure EP FAIL\n"); return -1;
        }
        serial_write("Configure EP OK\n");

        terminal_writeline("USB: Ready!");
        terminal_writeline("=== usb_init SUCCESS ===");

        /* Mark port as occupied so usb_hid_kbd skips it */
        if (connected_port >= 0 && connected_port < XHCI_MAX_PORTS)
            g_xhci_occupied_ports[connected_port] = 1;

        usb_parse_partition_table();
        terminal_writeline("USB: Storage ready.");
        return 0;
    }

    serial_write("No xHCI found\n");
    return -1;
}
