#include "keyboard.h"
#include "io.h"

static keyboard_state_t kb_state = {0, 0, 0, 0};

// Unshifted ASCII map
static const char scancode_ascii_unshifted[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
     0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
     0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
   '*',   0, ' '
};

// Shifted ASCII map
static const char scancode_ascii_shifted[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
  '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
     0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
     0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
   '*',   0, ' '
};

void keyboard_init(void) {
    outb(0x64, 0xAE); // Enable PS/2 keyboard
}

keyboard_state_t keyboard_get_state(void) {
    return kb_state;
}

char keyboard_get_char(void) {
    if ((inb(0x64) & 1) == 0) {
        return 0; // No key available
    }

    uint8_t scancode = inb(0x60);

    // Press vs Release Scancodes
    if (scancode & 0x80) {
        // Key Release
        uint8_t released_code = scancode & 0x7F;
        if (released_code == 0x2A || released_code == 0x36) { // Left or Right Shift
            kb_state.shift = 0;
        } else if (released_code == 0x1D) { // Ctrl
            kb_state.ctrl = 0;
        } else if (released_code == 0x38) { // Alt
            kb_state.alt = 0;
        }
        return 0;
    }

    // Key Press
    if (scancode == 0x2A || scancode == 0x36) { // Shift Press
        kb_state.shift = 1;
        return 0;
    }
    if (scancode == 0x1D) { // Ctrl Press
        kb_state.ctrl = 1;
        return 0;
    }
    if (scancode == 0x38) { // Alt Press
        kb_state.alt = 1;
        return 0;
    }
    if (scancode == 0x3A) { // Caps Lock Toggle
        kb_state.caps_lock = !kb_state.caps_lock;
        return 0;
    }

    // Tab key handling
    if (scancode == 0x0F) { // Tab
        return '\t';
    }

    // Character evaluation
    char ch = 0;
    int use_shifted = kb_state.shift;

    if (scancode < 128) {
        ch = scancode_ascii_unshifted[scancode];
        char ch_shifted = scancode_ascii_shifted[scancode];

        // If letter key (a-z)
        if (ch >= 'a' && ch <= 'z') {
            if (kb_state.caps_lock ^ kb_state.shift) {
                ch = ch_shifted; // Uppercase A-Z
            }
        } else {
            if (use_shifted) {
                ch = ch_shifted; // Symbols !@#$%^&*()_+{}|:"<>?~
            }
        }
    }

    return ch;
}
