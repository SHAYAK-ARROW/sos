#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>
#include <stddef.h>

void terminal_init(void);
void terminal_putchar(char c);
void terminal_write(const char* str);
void terminal_writeline(const char* str);
void terminal_clear(void);
void terminal_setcolor(uint8_t fg, uint8_t bg);
void terminal_scroll_up(void);
void terminal_scroll_down(void);
void terminal_scroll_exit(void);
int  terminal_in_scroll(void);

/* VGA Colors */
#define VGA_BLACK         0
#define VGA_BLUE          1
#define VGA_GREEN         2
#define VGA_CYAN          3
#define VGA_RED           4
#define VGA_MAGENTA       5
#define VGA_BROWN         6
#define VGA_LIGHT_GREY    7
#define VGA_DARK_GREY     8
#define VGA_LIGHT_BLUE    9
#define VGA_LIGHT_GREEN   10
#define VGA_LIGHT_CYAN    11
#define VGA_LIGHT_RED     12
#define VGA_LIGHT_MAGENTA 13
#define VGA_LIGHT_BROWN   14
#define VGA_WHITE         15

#endif
