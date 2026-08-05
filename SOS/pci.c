#include "pci.h"
#include "io.h"
#include "screen.h"
#include "usb_uhci.h"

static pci_device_t pci_devices[32];
static int num_pci_devices = 0;

uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, val);
}

static const char* pci_get_class_name(uint8_t class_id, uint8_t subclass_id, uint8_t interface_id) {
    if (class_id == 0x0C && subclass_id == 0x03) {
        if (interface_id == 0x00) return "USB UHCI Controller";
        if (interface_id == 0x10) return "USB OHCI Controller";
        if (interface_id == 0x20) return "USB EHCI (USB 2.0) Controller";
        if (interface_id == 0x30) return "USB xHCI (USB 3.0) Controller";
        return "USB Controller";
    }
    if (class_id == 0x01) return "Storage Controller";
    if (class_id == 0x03) return "Display Controller (VGA/HDMI)";
    if (class_id == 0x02) return "Network Controller";
    return "PCI Device";
}

void pci_scan_bus(void) {
    num_pci_devices = 0;
    screen_puts("Scanning PCI Bus:\n");
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_read_config_dword(bus, slot, func, 0x00);
                uint16_t vendor_id = vendor_device & 0xFFFF;
                uint16_t device_id = (vendor_device >> 16) & 0xFFFF;

                if (vendor_id == 0xFFFF) continue;

                uint32_t class_info = pci_read_config_dword(bus, slot, func, 0x08);
                uint8_t class_id = (class_info >> 24) & 0xFF;
                uint8_t subclass_id = (class_info >> 16) & 0xFF;
                uint8_t interface_id = (class_info >> 8) & 0xFF;

                if (num_pci_devices < 32) {
                    pci_devices[num_pci_devices].bus = bus;
                    pci_devices[num_pci_devices].slot = slot;
                    pci_devices[num_pci_devices].func = func;
                    pci_devices[num_pci_devices].vendor_id = vendor_id;
                    pci_devices[num_pci_devices].device_id = device_id;
                    pci_devices[num_pci_devices].class_id = class_id;
                    pci_devices[num_pci_devices].subclass_id = subclass_id;
                    pci_devices[num_pci_devices].interface_id = interface_id;

                    screen_puts("  Found: ");
                    screen_puts(pci_get_class_name(class_id, subclass_id, interface_id));
                    screen_puts(" (Vendor: 0x");
                    screen_putnum(vendor_id, 16);
                    screen_puts(", Device: 0x");
                    screen_putnum(device_id, 16);
                    screen_puts(")\n");

                    // Bug 2 Fix: Do NOT call uhci_init() during shell "pci" scan if already initialized!
                    if (class_id == 0x0C && subclass_id == 0x03 && interface_id == 0x00) {
                        if (uhci_get_io_base() != 0) {
                            uhci_check_ports(); // Controller already running -> just check ports
                        } else {
                            uhci_init(&pci_devices[num_pci_devices]); // Initial boot setup
                        }
                    }

                    num_pci_devices++;
                }
            }
        }
    }
}

void pci_init(void) {
    // Boot-time PCI Scan & Controller Initialization
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_read_config_dword(bus, slot, func, 0x00);
                uint16_t vendor_id = vendor_device & 0xFFFF;
                uint16_t device_id = (vendor_device >> 16) & 0xFFFF;

                if (vendor_id == 0xFFFF) continue;

                uint32_t class_info = pci_read_config_dword(bus, slot, func, 0x08);
                uint8_t class_id = (class_info >> 24) & 0xFF;
                uint8_t subclass_id = (class_info >> 16) & 0xFF;
                uint8_t interface_id = (class_info >> 8) & 0xFF;

                if (class_id == 0x0C && subclass_id == 0x03 && interface_id == 0x00) {
                    if (num_pci_devices < 32) {
                        pci_devices[num_pci_devices].bus = bus;
                        pci_devices[num_pci_devices].slot = slot;
                        pci_devices[num_pci_devices].func = func;
                        pci_devices[num_pci_devices].vendor_id = vendor_id;
                        pci_devices[num_pci_devices].device_id = device_id;
                        pci_devices[num_pci_devices].class_id = class_id;
                        pci_devices[num_pci_devices].subclass_id = subclass_id;
                        pci_devices[num_pci_devices].interface_id = interface_id;
                        
                        uhci_init(&pci_devices[num_pci_devices]);
                        num_pci_devices++;
                    }
                }
            }
        }
    }
}
