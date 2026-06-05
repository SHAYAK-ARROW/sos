#include "paging.h"
#include "pmm.h"
#include "../drivers/terminal.h"
#include <stdint.h>

/*
 * paging.c — x86 32-bit Identity-Map Paging
 *
 * MMIO coverage: 0x80000000–0xFFFFFFFF (2GB upper half)
 * Real hardware এ xHCI BAR যেকোনো জায়গায় থাকতে পারে,
 * তাই 0x80000000 থেকে map করা হচ্ছে।
 */

__attribute__((aligned(4096))) static uint32_t page_directory[1024];

/* 0x80000000–0xFFFFFFFF = PDE 512..1023 = 512 entries */
#define MMIO_PDE_START 512
#define MMIO_PDE_COUNT 512
__attribute__((aligned(4096))) static uint32_t mmio_pts[MMIO_PDE_COUNT][1024];

void paging_init(void) {

    /* Step 1: Low memory identity map (0x00000000–0x7FFFFFFF, 2GB, 512 PDEs)
     * Use static tables so we don't exhaust PMM — covers all possible
     * xHCI BAR0 addresses below 0x80000000 on real hardware. */
    static uint32_t low_pts[512][1024] __attribute__((aligned(4096)));
    for (int i = 0; i < 512; i++) {
        for (int j = 0; j < 1024; j++) {
            uint32_t phys = (uint32_t)(i * 1024 * 4096) + (uint32_t)(j * 4096);
            low_pts[i][j] = phys | 0x03u; /* Present | R/W */
        }
        page_directory[i] = (uint32_t)(uintptr_t)low_pts[i] | 0x03u;
    }

    /* Step 2: MMIO region (0x80000000–0xFFFFFFFF) — cache disabled */
    for (int i = 0; i < MMIO_PDE_COUNT; i++) {
        uint32_t *pt = mmio_pts[i];
        for (int j = 0; j < 1024; j++) {
            uint32_t phys = (uint32_t)(MMIO_PDE_START + i) * 1024u * 4096u
                          + (uint32_t)j * 4096u;
            pt[j] = phys | 0x1Bu; /* Present|RW|PWT|PCD */
        }
        page_directory[MMIO_PDE_START + i] = (uint32_t)(uintptr_t)pt | 0x1Bu;
    }

    /* Step 3: CR3 + enable paging */
    uint32_t pd_phys = (uint32_t)(uintptr_t)page_directory;
    __asm__ volatile (
        "mov %0, %%cr3\n"
        "mov %%cr0, %%eax\n"
        "or $0x80000000, %%eax\n"
        "mov %%eax, %%cr0\n"
        : : "r"(pd_phys) : "eax"
    );

    terminal_writeline("Paging initialized.");
}

void paging_map_page(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    (void)virt_addr; (void)phys_addr; (void)flags;
}
uint32_t paging_get_physical(uint32_t virt_addr) { return virt_addr; }
