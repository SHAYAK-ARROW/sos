#include "screen.h"

static uint16_t *vga_buffer = (uint16_t *)VGA_ADDRESS;
static uint8_t cursor_x = 0;
static uint8_t cursor_y = 0;
static uint8_t current_color = (COLOR_BLACK << 4) | COLOR_WHITE;

static uint16_t vga_entry(unsigned char uc, uint8_t color) {
    return (uint16_t) uc | ((uint16_t) color << 8);
}

void screen_set_color(uint8_t fg, uint8_t bg) {
    current_color = (bg << 4) | (fg & 0x0F);
}

void screen_clear(void) {
    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            const int index = y * VGA_WIDTH + x;
            vga_buffer[index] = vga_entry(' ', current_color);
        }
    }
    cursor_x = 0;
    cursor_y = 0;
}

void screen_init(void) {
    screen_set_color(COLOR_WHITE, COLOR_BLACK);
    screen_clear();
}

static void scroll(void) {
    if (cursor_y >= VGA_HEIGHT) {
        for (int y = 0; y < VGA_HEIGHT - 1; y++) {
            for (int x = 0; x < VGA_WIDTH; x++) {
                vga_buffer[y * VGA_WIDTH + x] = vga_buffer[(y + 1) * VGA_WIDTH + x];
            }
        }
        for (int x = 0; x < VGA_WIDTH; x++) {
            vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', current_color);
        }
        cursor_y = VGA_HEIGHT - 1;
    }
}

void screen_putc(char c) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\t') {
        cursor_x = (cursor_x + 4) & ~3;
        if (cursor_x >= VGA_WIDTH) {
            cursor_x = 0;
            cursor_y++;
        }
    } else if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            const int index = cursor_y * VGA_WIDTH + cursor_x;
            vga_buffer[index] = vga_entry(' ', current_color);
        } else if (cursor_y > 0) {
            cursor_y--;
            cursor_x = VGA_WIDTH - 1;
            const int index = cursor_y * VGA_WIDTH + cursor_x;
            vga_buffer[index] = vga_entry(' ', current_color);
        }
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c >= ' ') {
        const int index = cursor_y * VGA_WIDTH + cursor_x;
        vga_buffer[index] = vga_entry(c, current_color);
        cursor_x++;
        if (cursor_x >= VGA_WIDTH) {
            cursor_x = 0;
            cursor_y++;
        }
    }
    scroll();
}

void screen_puts(const char *str) {
    while (*str) {
        screen_putc(*str++);
    }
}

void screen_putnum(uint32_t n, uint8_t base) {
    char buf[32];
    int i = 0;
    if (n == 0) {
        screen_putc('0');
        return;
    }
    while (n > 0) {
        int rem = n % base;
        buf[i++] = (rem < 10) ? (rem + '0') : (rem - 10 + 'A');
        n /= base;
    }
    while (i > 0) {
        screen_putc(buf[--i]);
    }
}
