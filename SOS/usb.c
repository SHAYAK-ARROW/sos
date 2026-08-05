#include "usb.h"
#include "screen.h"

static usb_device_t connected_usb_devices[8];
static int num_usb_devices = 0;

void usb_init(void) {
    connected_usb_devices[0].address = 1;
    connected_usb_devices[0].vendor_id = 0x0781;
    connected_usb_devices[0].product_id = 0x5567;
    connected_usb_devices[0].class_code = USB_CLASS_MASS_STORAGE;
    connected_usb_devices[0].subclass_code = USB_SUBCLASS_SCSI;
    connected_usb_devices[0].protocol_code = USB_PROTOCOL_BOT;
    connected_usb_devices[0].is_connected = 1;

    connected_usb_devices[1].address = 2;
    connected_usb_devices[1].vendor_id = 0x046D;
    connected_usb_devices[1].product_id = 0xC31C;
    connected_usb_devices[1].class_code = USB_CLASS_HID;
    connected_usb_devices[1].is_connected = 1;

    num_usb_devices = 2;
}

void usb_poll(void) {
}

int usb_read_sector(uint32_t lba, uint8_t *buffer) {
    (void)lba;
    for (int i = 0; i < 512; i++) {
        buffer[i] = 0;
    }
    return 1;
}

int usb_write_sector(uint32_t lba, const uint8_t *buffer) {
    (void)lba;
    (void)buffer;
    return 1;
}

char usb_hid_decode_scancode(uint8_t keycode) {
    if (keycode >= 0x04 && keycode <= 0x1D) {
        return 'a' + (keycode - 0x04);
    }
    if (keycode >= 0x1E && keycode <= 0x27) {
        if (keycode == 0x27) return '0';
        return '1' + (keycode - 0x1E);
    }
    if (keycode == 0x28) return '\n';
    if (keycode == 0x2A) return '\b';
    if (keycode == 0x2C) return ' ';
    return 0;
}
