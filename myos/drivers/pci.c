#include <stdint.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

extern void terminal_writeline(const char* data);

static inline void outl(uint16_t port, uint32_t val) {
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    // FIX: cast each field to uint32_t BEFORE shifting to avoid UB
    uint32_t address = ((uint32_t)bus  << 16) |
                       ((uint32_t)slot << 11) |
                       ((uint32_t)func <<  8) |
                       ((uint32_t)(offset & 0xFC)) |
                       0x80000000U;
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void check_all_pci_buses() {
    terminal_writeline("Scanning PCI Bus 0 (Safe Mode)...\n");

    uint8_t bus = 0;

    // FIX: disable interrupts once around the whole scan, not per slot
    asm volatile("cli");

    for (uint8_t slot = 0; slot < 32; slot++) {
        uint32_t reg0 = pci_read_config(bus, slot, 0, 0);
        uint16_t vendor_id = reg0 & 0xFFFF;

        if (vendor_id == 0xFFFF) {
            continue; // No device present
        }

        uint32_t reg8 = pci_read_config(bus, slot, 0, 0x08);
        uint8_t class_code = (reg8 >> 24) & 0xFF;
        uint8_t subclass   = (reg8 >> 16) & 0xFF;

        if (class_code == 0x0C && subclass == 0x03) {
            terminal_writeline("Found USB Controller!\n");
        }
    }

  //  asm volatile("sti");

    terminal_writeline("PCI Scan Finished.\n");
}
