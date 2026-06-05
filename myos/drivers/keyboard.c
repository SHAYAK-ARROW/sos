/*
 * keyboard.c — Unified Keyboard Subsystem
 * ========================================
 * Tries USB HID keyboard first (via usb_hid_kbd.c).
 * If no USB keyboard is detected, falls back to the legacy PS/2
 * controller at I/O ports 0x60 / 0x64.
 *
 * All callers use keyboard_getchar() / keyboard_readline() and
 * never need to know which backend is active.
 */

#include "keyboard.h"
#include "usb_hid_kbd.h"
#include "terminal.h"
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* PS/2 Backend                                                       */
/* ------------------------------------------------------------------ */
#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64

static const char scancode_map[] = {
    0,   27,  '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,   '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,  ' '
};

static const char scancode_shift[] = {
    0,   27,  '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,   'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0,  ' '
};

static int ps2_shift   = 0;
static int ps2_caps    = 0;

static inline uint8_t ps2_inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static char ps2_getchar(void) {
    while (1) {
        if (!(ps2_inb(PS2_STATUS_PORT) & 1)) continue;
        uint8_t sc = ps2_inb(PS2_DATA_PORT);

        /* Key release */
        if (sc & 0x80) {
            uint8_t rel = sc & 0x7F;
            if (rel == 0x2A || rel == 0x36) ps2_shift = 0;
            continue;
        }

        if (sc == 0x2A || sc == 0x36) { ps2_shift = 1; continue; }
        if (sc == 0x3A)               { ps2_caps ^= 1; continue; }

        if (sc >= (uint8_t)sizeof(scancode_map)) continue;

        char c = ps2_shift ? scancode_shift[sc] : scancode_map[sc];
        if (!c) continue;

        if (ps2_caps && c >= 'a' && c <= 'z') c -= 32;
        if (ps2_caps && c >= 'A' && c <= 'Z' && !ps2_shift) c += 32;
        return c;
    }
}

static void ps2_readline(char *buf, int maxlen) {
    int i = 0;
    while (i < maxlen - 1) {
        char c = ps2_getchar();
        if (c == '\n') { terminal_putchar('\n'); break; }
        if (c == '\b') {
            if (i > 0) { i--; terminal_putchar('\b'); }
            continue;
        }
        buf[i++] = c;
        terminal_putchar(c);
    }
    buf[i] = '\0';
}

/* ------------------------------------------------------------------ */
/* Active backend selection                                           */
/* ------------------------------------------------------------------ */
typedef enum { BACKEND_PS2, BACKEND_USB_HID } kbd_backend_t;
static kbd_backend_t g_backend = BACKEND_PS2;

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void keyboard_init(void) {
    ps2_shift = 0;
    ps2_caps  = 0;

    /* Try USB HID first */
    if (usb_hid_kbd_init() == 0) {
        g_backend = BACKEND_USB_HID;
        terminal_write("[Keyboard] Backend: ");
        terminal_writeline("USB HID");
    } else {
        g_backend = BACKEND_PS2;
        terminal_write("[Keyboard] Backend: ");
        terminal_writeline("PS/2");
    }
}

char keyboard_getchar(void) {
    if (g_backend == BACKEND_USB_HID)
        return usb_hid_kbd_getchar();
    return ps2_getchar();
}

void keyboard_readline(char *buf, int maxlen) {
    if (g_backend == BACKEND_USB_HID)
        usb_hid_kbd_readline(buf, maxlen);
    else
        ps2_readline(buf, maxlen);
}

const char *keyboard_backend_name(void) {
    return (g_backend == BACKEND_USB_HID) ? "USB HID" : "PS/2";
}
