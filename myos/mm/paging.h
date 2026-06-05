#ifndef PAGING_H
#define PAGING_H

/*
 * paging.h — Identity-Map Paging (i386, 32-bit)
 *
 * Call paging_init() early in kernel_main, after pmm_init() and before
 * any driver that needs to pass physical addresses to hardware (xHCI).
 */

#include <stdint.h>

/* Page entry flag bits (low 12 bits of a PTE/PDE) */
#define PAGE_PRESENT   (1u << 0)
#define PAGE_WRITABLE  (1u << 1)
#define PAGE_USER      (1u << 2)   /* set this for user-space pages */
#define PAGE_NOCACHE   (1u << 4)   /* useful for MMIO mappings */

/*
 * paging_init()
 *
 * Builds a full identity-map page directory + page tables covering
 * the first 1 GiB of physical address space, loads CR3, and enables
 * the CR0.PG bit.
 *
 * After this call:
 *   virtual address VA == physical address PA  (for VA < 1 GiB)
 *   paging is ON (CR0.PG = 1)
 */
void paging_init(void);

/*
 * paging_map_page(virt, phys, flags)
 *
 * Maps a single 4 KiB page.  Only works within the first 1 GiB (where
 * page tables already exist).
 * flags: combination of PAGE_PRESENT | PAGE_WRITABLE | PAGE_NOCACHE etc.
 */
void paging_map_page(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);

/*
 * paging_get_physical(virt)
 *
 * Returns the physical address that virt_addr is currently mapped to,
 * or 0 if the mapping is not present.
 * Useful for verifying DMA buffer addresses before handing them to xHCI.
 */
uint32_t paging_get_physical(uint32_t virt_addr);

#endif /* PAGING_H */
