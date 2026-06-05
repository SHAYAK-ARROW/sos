#ifndef USB_HID_KBD_H
#define USB_HID_KBD_H

/*
 * usb_hid_kbd.h — USB HID Boot-Protocol Keyboard Driver
 *
 * Targets xHCI (USB 3.x) controllers on bare-metal x86.
 * Uses the HID Boot Protocol so no HID report descriptor parsing
 * is needed — every compliant USB keyboard supports it.
 *
 * Public API mirrors the PS/2 keyboard API so the rest of the
 * kernel can call whichever one is available.
 */

#include <stdint.h>

/*
 * usb_hid_kbd_init()
 *
 * Scans all PCI buses for an xHCI controller, then walks the
 * connected USB devices looking for a HID Boot Keyboard
 * (class=0x03, subclass=0x01, protocol=0x01).
 *
 * Returns  0  on success (keyboard found and configured).
 * Returns -1  if no USB HID keyboard was detected.
 *
 * Must be called after the xHCI host controller has already been
 * reset and started (i.e. after usb_init() in usb.c, or standalone
 * if usb.c is not used).
 */
int usb_hid_kbd_init(void);

/*
 * usb_hid_kbd_present()
 *
 * Returns 1 if a USB HID keyboard was successfully initialised,
 * 0 otherwise.  Use this to decide whether to fall back to PS/2.
 */
int usb_hid_kbd_present(void);

/*
 * usb_hid_kbd_getchar()
 *
 * Blocking read: polls the keyboard's interrupt-IN endpoint until
 * a key-press is detected, then returns its ASCII value.
 * Non-printable / unrecognised keycodes return 0 — callers must
 * handle that (just call again).
 *
 * Modifier keys (Shift, Ctrl, Alt, CapsLock) are handled internally.
 */
char usb_hid_kbd_getchar(void);

/*
 * usb_hid_kbd_readline()
 *
 * Reads a line of text (terminated by Enter) into buf.
 * maxlen includes the NUL terminator.
 * Backspace erases the last character both in the buffer and on
 * the terminal.
 */
void usb_hid_kbd_readline(char *buf, int maxlen);

#endif /* USB_HID_KBD_H */
