#include "pmm.h"
#include "../drivers/terminal.h"
#include <stddef.h>

#define PAGE_SIZE       4096u
#define PAGE_SHIFT      12u
#define MAX_PHYS_ADDR   0x100000000ULL
#define MAX_PAGES       (MAX_PHYS_ADDR / PAGE_SIZE)
#define BITMAP_ENTRIES  (MAX_PAGES / 32u)

extern uint32_t _kernel_start_phys;
extern uint32_t _kernel_end_phys;

static uint32_t g_bitmap[BITMAP_ENTRIES];
static uint32_t g_total_pages = 0;
static uint32_t g_free_pages  = 0;
static uint32_t g_last_alloc  = 0;
static int      g_initialized = 0;

static inline void bitmap_set(uint32_t page) { g_bitmap[page / 32] |=  (1u << (page % 32)); }
static inline void bitmap_clear(uint32_t page) { g_bitmap[page / 32] &= ~(1u << (page % 32)); }
static inline int bitmap_test(uint32_t page) { return (g_bitmap[page / 32] >> (page % 32)) & 1u; }
static inline uint32_t addr_to_page(uint32_t addr) { return addr >> PAGE_SHIFT; }
static inline uint32_t page_to_addr(uint32_t page) { return page << PAGE_SHIFT; }

static void print_uint32_hex(uint32_t val) {
    char buf[11]; buf[0] = '0'; buf[1] = 'x'; buf[10] = '\0';
    for (int i = 9; i >= 2; i--) {
        uint32_t nibble = val & 0xF;
        buf[i] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        val >>= 4;
    }
    terminal_write(buf);
}

static void print_uint32_dec(uint32_t val) {
    char buf[11]; int i = 10; buf[10] = '\0';
    if (val == 0) { terminal_write("0"); return; }
    while (val > 0 && i > 0) {
        buf[--i] = (char)('0' + (val % 10));
        val /= 10;
    }
    terminal_write(&buf[i]);
}

/* -- Kernel boundary detection --------------------------------------- */
static uint32_t get_kernel_start(void) {
    uint32_t s = (uint32_t)&_kernel_start_phys;
    return s & ~(PAGE_SIZE - 1u);
}

static uint32_t get_kernel_end(void) {
    uint32_t e = (uint32_t)&_kernel_end_phys;
    return (e + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
}

static void mark_region_free(uint64_t base, uint64_t len) {
    if (base >= MAX_PHYS_ADDR) return;
    uint64_t end = base + len;
    if (end > MAX_PHYS_ADDR) end = MAX_PHYS_ADDR;
    uint32_t first = (uint32_t)((base + PAGE_SIZE - 1u) & ~(uint64_t)(PAGE_SIZE - 1u));
    uint32_t last  = (uint32_t)(end & ~(uint64_t)(PAGE_SIZE - 1u));
    if (first >= last) return;
    uint32_t page_start = addr_to_page(first);
    uint32_t page_end   = addr_to_page(last);
    for (uint32_t p = page_start; p < page_end; p++) {
        if (bitmap_test(p)) {
            bitmap_clear(p);
            g_free_pages++;
        }
    }
}

static void mark_region_used(uint32_t base, uint32_t len) {
    uint32_t first = base & ~(PAGE_SIZE - 1u);
    uint32_t end   = base + len;
    uint32_t last  = (end + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    uint32_t page_start = addr_to_page(first);
    uint32_t page_end   = addr_to_page(last);
    if (page_end > (uint32_t)MAX_PAGES) page_end = (uint32_t)MAX_PAGES;
    for (uint32_t p = page_start; p < page_end; p++) {
        if (!bitmap_test(p)) {
            bitmap_set(p);
            if (g_free_pages > 0) g_free_pages--;
        }
    }
}

static void pmm_panic(const char *msg) {
    terminal_write("[PMM FATAL] ");
    terminal_writeline(msg);
    __asm__ volatile ("cli; hlt");
    while (1) {}
}

void pmm_init(multiboot_info_t *mbd, uint32_t magic) {
    if (magic != MULTIBOOT_MAGIC)
        pmm_panic("Bad Multiboot magic.");
    if (!(mbd->flags & MULTIBOOT_FLAG_MEMMAP))
        pmm_panic("Multiboot memory map not available.");

    for (uint32_t i = 0; i < BITMAP_ENTRIES; i++)
        g_bitmap[i] = 0xFFFFFFFFu;
    g_free_pages  = 0;
    g_total_pages = (uint32_t)MAX_PAGES;
    g_last_alloc  = 0;

    terminal_writeline("[PMM] Parsing Multiboot memory map:");
    uint8_t *mmap_ptr = (uint8_t *)(uintptr_t)mbd->mmap_addr;
    uint8_t *mmap_end = mmap_ptr + mbd->mmap_length;

    while (mmap_ptr < mmap_end) {
        mmap_entry_t *e = (mmap_entry_t *)mmap_ptr;
        uint64_t base = ((uint64_t)e->base_hi << 32) | e->base_lo;
        uint64_t len  = ((uint64_t)e->len_hi  << 32) | e->len_lo;

        terminal_write("  base="); print_uint32_hex(e->base_lo);
        terminal_write(" len="); print_uint32_hex(e->len_lo);
        terminal_write(" type="); print_uint32_dec(e->type);
        terminal_write(" -> ");

        if (e->type == MULTIBOOT_MEMORY_AVAILABLE && e->base_hi == 0) {
            mark_region_free(base, len);
            terminal_writeline("FREE");
        } else {
            terminal_writeline("reserved");
        }
        mmap_ptr += e->size + sizeof(e->size);
    }

    mark_region_used(0x00000000u, 0x00100000u);
    uint32_t k_start = get_kernel_start();
    uint32_t k_end   = get_kernel_end();
    mark_region_used(k_start, k_end - k_start);
    
    terminal_write("[PMM] Kernel occupies ");
    print_uint32_hex(k_start);
    terminal_write(" - ");
    print_uint32_hex(k_end);
    terminal_writeline("");

    uint32_t bm_start = (uint32_t)(uintptr_t)g_bitmap;
    uint32_t bm_size  = (uint32_t)sizeof(g_bitmap);
    mark_region_used(bm_start, bm_size);
    
    g_initialized = 1;
    terminal_write("[PMM] Init complete. Free pages: ");
    print_uint32_dec(g_free_pages);
    terminal_writeline("");
}

uint32_t pmm_alloc_page(void) {
    if (!g_initialized || g_free_pages == 0) return 0;
    uint32_t start = (g_last_alloc < 1) ? 1 : g_last_alloc;
    uint32_t p = start;
    do {
        if (!bitmap_test(p)) {
            bitmap_set(p);
            g_free_pages--;
            g_last_alloc = p + 1;
            return page_to_addr(p);
        }
        p++;
        if (p >= (uint32_t)MAX_PAGES) p = 1;
    } while (p != start);
    return 0;
}

uint32_t pmm_alloc_pages(uint32_t count) {
    if (!g_initialized || count == 0 || g_free_pages < count) return 0;
    uint32_t run_start = 1;
    while (run_start + count <= (uint32_t)MAX_PAGES) {
        while (run_start < (uint32_t)MAX_PAGES && bitmap_test(run_start)) run_start++;
        if (run_start + count > (uint32_t)MAX_PAGES) break;
        
        uint32_t run_len = 0;
        while (run_len < count && run_start + run_len < (uint32_t)MAX_PAGES && !bitmap_test(run_start + run_len)) {
            run_len++;
        }

        if (run_len == count) {
            for (uint32_t i = 0; i < count; i++) {
                bitmap_set(run_start + i);
                g_free_pages--;
            }
            g_last_alloc = run_start + count;
            return page_to_addr(run_start);
        }
        run_start += run_len + 1;
    }
    return 0;
}

void pmm_free_page(uint32_t phys_addr) {
    if (phys_addr == 0) return;
    uint32_t p = addr_to_page(phys_addr);
    if (p >= (uint32_t)MAX_PAGES) return;
    if (!bitmap_test(p)) return;
    bitmap_clear(p);
    g_free_pages++;
    if (p < g_last_alloc) g_last_alloc = p;
}

void pmm_free_pages(uint32_t phys_addr, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) pmm_free_page(phys_addr + i * PAGE_SIZE);
}

void pmm_print_stats(void) {
    uint32_t used = g_total_pages - g_free_pages;
    terminal_write("[PMM] Total: "); print_uint32_dec(g_total_pages);
    terminal_write("  Free: ");      print_uint32_dec(g_free_pages);
    terminal_write("  Used: ");      print_uint32_dec(used);
    terminal_writeline("");
}
