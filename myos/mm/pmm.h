#ifndef PMM_H
#define PMM_H

/*
 * pmm.h — Physical Memory Manager (Bitmap Allocator)
 *
 * Manages 4 KiB pages of physical memory.
 *
 * Usage order:
 *   1. pmm_init(mbd)          — call once, very early in kernel_main,
 *                               before anything else allocates memory.
 *   2. pmm_alloc_page()       — returns a physical address (4 KiB aligned).
 *   3. pmm_alloc_pages(n)     — returns n contiguous pages (needed for DMA).
 *   4. pmm_free_page(addr)    — returns one page to the pool.
 *   5. pmm_free_pages(addr,n) — returns n contiguous pages.
 *   6. pmm_print_stats()      — debug helper.
 */

#include <stdint.h>

/* Multiboot info structures (Multiboot 1 spec, §3.3)
 * We define only what we actually use to keep the header self-contained.
 * If you already have a full multiboot.h, replace this block with
 * #include "multiboot.h" and remove the duplicate typedefs.             */

#define MULTIBOOT_MAGIC          0x2BADB002u
#define MULTIBOOT_FLAG_MEMMAP    (1u << 6)   /* mmap_* fields valid */
#define MULTIBOOT_MEMORY_AVAILABLE   1

typedef struct {
    uint32_t flags;

    /* mem_lower / mem_upper (kilobytes) — flags bit 0 */
    uint32_t mem_lower;
    uint32_t mem_upper;

    uint32_t boot_device;   /* flags bit 1 */
    uint32_t cmdline;       /* flags bit 2 */

    /* Modules — flags bit 3 */
    uint32_t mods_count;
    uint32_t mods_addr;

    uint32_t syms[4];       /* flags bits 4 / 5 */

    /* Memory map — flags bit 6  ← the part we care about */
    uint32_t mmap_length;   /* total byte length of the mmap buffer */
    uint32_t mmap_addr;     /* physical address of first mmap_entry_t */

    /* (remaining fields omitted — we don't need them) */
} __attribute__((packed)) multiboot_info_t;

/* Each entry in the memory map.
 * Note: the 'size' field does NOT include itself.  The next entry is at
 *   (uint8_t*)entry + entry->size + sizeof(entry->size)                 */
typedef struct {
    uint32_t size;          /* size of the rest of this entry (≥ 20)  */
    uint32_t base_lo;       /* base address, low  32 bits              */
    uint32_t base_hi;       /* base address, high 32 bits              */
    uint32_t len_lo;        /* length,        low  32 bits             */
    uint32_t len_hi;        /* length,        high 32 bits             */
    uint32_t type;          /* 1 = available RAM; anything else = reserved */
} __attribute__((packed)) mmap_entry_t;

/* ── Public API ──────────────────────────────────────────────────────── */

/*
 * pmm_init(mbd, magic)
 *
 * Parses the Multiboot memory map and marks every usable page as free,
 * then marks the pages occupied by the kernel itself (and the bitmap) as
 * used so they are never handed out.
 *
 * Panics (prints error + halts) if:
 *   - magic != MULTIBOOT_MAGIC
 *   - the memory-map flag is not set in mbd->flags
 */
void pmm_init(multiboot_info_t *mbd, uint32_t magic);

/*
 * pmm_alloc_page()
 *
 * Allocates one free physical page (4 KiB).
 * Returns its physical address, which is always 4 KiB aligned.
 * Returns 0 if out of memory (0 is never a valid page address here
 * because the first 1 MiB is always marked reserved).
 */
uint32_t pmm_alloc_page(void);

/*
 * pmm_alloc_pages(count)
 *
 * Allocates 'count' *contiguous* physical pages.
 * Needed for xHCI data structures that require physically contiguous,
 * aligned buffers (transfer rings, event ring segment tables, etc.).
 *
 * Returns physical base address of the block, or 0 on failure.
 * The returned address is 4 KiB aligned; for larger alignments
 * (e.g. 64-byte xHCI structures) the caller must verify — in practice
 * 4 KiB pages always satisfy any sub-page alignment requirement.
 */
uint32_t pmm_alloc_pages(uint32_t count);

/*
 * pmm_free_page(phys_addr)
 *
 * Returns one page to the free pool.
 * phys_addr must be 4 KiB aligned.
 */
void pmm_free_page(uint32_t phys_addr);

/*
 * pmm_free_pages(phys_addr, count)
 *
 * Returns 'count' contiguous pages starting at phys_addr.
 */
void pmm_free_pages(uint32_t phys_addr, uint32_t count);

/*
 * pmm_print_stats()
 *
 * Prints free/used/total page counts to the terminal.
 * Useful for debugging before launching the shell.
 */
void pmm_print_stats(void);

#endif /* PMM_H */
