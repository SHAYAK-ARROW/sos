#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

#include <stdint.h>
#include <stdarg.h>

void putchar(char c);
void puts(const char *str);
int printf(const char *format, ...);
int sprintf(char *str, const char *format, ...);

#endif
