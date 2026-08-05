#include "stdio.h"
#include "../screen.h"

void putchar(char c) {
    screen_putc(c);
}

void puts(const char *str) {
    screen_puts(str);
    screen_putc('\n');
}

// Helper: Format unsigned integer into string buffer
static int format_uint_to_buf(uint32_t val, uint8_t base, int uppercase, char *buf) {
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char *digits = uppercase ? digits_upper : digits_lower;

    char temp[32];
    int temp_idx = 0;

    if (val == 0) {
        temp[temp_idx++] = '0';
    } else {
        while (val > 0) {
            temp[temp_idx++] = digits[val % base];
            val /= base;
        }
    }

    // Reverse into buf
    int buf_idx = 0;
    while (temp_idx > 0) {
        buf[buf_idx++] = temp[--temp_idx];
    }
    buf[buf_idx] = '\0';
    return buf_idx;
}

int printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    
    while (*format) {
        if (*format == '%') {
            format++;

            // 1. Check for optional '0' zero-padding flag
            int pad_zero = 0;
            if (*format == '0') {
                pad_zero = 1;
                format++;
            }

            // 2. Parse optional numeric width field (e.g. "02", "08", "10")
            int width = 0;
            while (*format >= '0' && *format <= '9') {
                width = width * 10 + (*format - '0');
                format++;
            }

            char num_buf[32];

            // 3. Read conversion character (s/d/i/u/x/X/c/%)
            if (*format == 's') {
                char *s = va_arg(args, char *);
                if (!s) s = "(null)";
                int len = 0;
                while (s[len]) len++;

                // Pad spaces if width > len
                if (width > len) {
                    for (int p = 0; p < width - len; p++) {
                        screen_putc(' ');
                    }
                }
                screen_puts(s);

            } else if (*format == 'd' || *format == 'i') {
                int d = va_arg(args, int);
                int is_neg = 0;
                uint32_t val = (uint32_t)d;

                if (d < 0) {
                    is_neg = 1;
                    val = (uint32_t)(-d);
                }

                int len = format_uint_to_buf(val, 10, 0, num_buf);
                int total_len = len + (is_neg ? 1 : 0);

                if (is_neg && pad_zero) {
                    screen_putc('-'); // Print sign before zero-padding
                }

                if (width > total_len) {
                    char pad_char = pad_zero ? '0' : ' ';
                    for (int p = 0; p < width - total_len; p++) {
                        screen_putc(pad_char);
                    }
                }

                if (is_neg && !pad_zero) {
                    screen_putc('-'); // Print sign after space-padding
                }

                screen_puts(num_buf);

            } else if (*format == 'u') {
                uint32_t u = va_arg(args, uint32_t);
                int len = format_uint_to_buf(u, 10, 0, num_buf);

                if (width > len) {
                    char pad_char = pad_zero ? '0' : ' ';
                    for (int p = 0; p < width - len; p++) {
                        screen_putc(pad_char);
                    }
                }

                screen_puts(num_buf);

            } else if (*format == 'x' || *format == 'X') {
                uint32_t x = va_arg(args, uint32_t);
                int uppercase = (*format == 'X');
                int len = format_uint_to_buf(x, 16, uppercase, num_buf);

                if (width > len) {
                    char pad_char = pad_zero ? '0' : ' ';
                    for (int p = 0; p < width - len; p++) {
                        screen_putc(pad_char);
                    }
                }

                screen_puts(num_buf);

            } else if (*format == 'c') {
                char c = (char)va_arg(args, int);
                screen_putc(c);

            } else if (*format == '%') {
                screen_putc('%');
            } else {
                // Unknown format specifier: print verbatim
                screen_putc('%');
                if (*format) screen_putc(*format);
            }
        } else {
            screen_putc(*format);
        }

        if (*format) format++;
    }
    
    va_end(args);
    return 0;
}

int sprintf(char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);

    char *dest = str;

    while (*format) {
        if (*format == '%') {
            format++;

            int pad_zero = 0;
            if (*format == '0') {
                pad_zero = 1;
                format++;
            }

            int width = 0;
            while (*format >= '0' && *format <= '9') {
                width = width * 10 + (*format - '0');
                format++;
            }

            char num_buf[32];

            if (*format == 's') {
                char *s = va_arg(args, char *);
                if (!s) s = "(null)";
                int len = 0;
                while (s[len]) len++;

                if (width > len) {
                    for (int p = 0; p < width - len; p++) *dest++ = ' ';
                }
                while (*s) *dest++ = *s++;

            } else if (*format == 'd' || *format == 'i') {
                int d = va_arg(args, int);
                int is_neg = 0;
                uint32_t val = (uint32_t)d;

                if (d < 0) {
                    is_neg = 1;
                    val = (uint32_t)(-d);
                }

                int len = format_uint_to_buf(val, 10, 0, num_buf);
                int total_len = len + (is_neg ? 1 : 0);

                if (is_neg && pad_zero) *dest++ = '-';

                if (width > total_len) {
                    char pad_char = pad_zero ? '0' : ' ';
                    for (int p = 0; p < width - total_len; p++) *dest++ = pad_char;
                }

                if (is_neg && !pad_zero) *dest++ = '-';

                char *p = num_buf;
                while (*p) *dest++ = *p++;

            } else if (*format == 'u') {
                uint32_t u = va_arg(args, uint32_t);
                int len = format_uint_to_buf(u, 10, 0, num_buf);

                if (width > len) {
                    char pad_char = pad_zero ? '0' : ' ';
                    for (int p = 0; p < width - len; p++) *dest++ = pad_char;
                }

                char *p = num_buf;
                while (*p) *dest++ = *p++;

            } else if (*format == 'x' || *format == 'X') {
                uint32_t x = va_arg(args, uint32_t);
                int uppercase = (*format == 'X');
                int len = format_uint_to_buf(x, 16, uppercase, num_buf);

                if (width > len) {
                    char pad_char = pad_zero ? '0' : ' ';
                    for (int p = 0; p < width - len; p++) *dest++ = pad_char;
                }

                char *p = num_buf;
                while (*p) *dest++ = *p++;

            } else if (*format == 'c') {
                char c = (char)va_arg(args, int);
                *dest++ = c;

            } else if (*format == '%') {
                *dest++ = '%';
            } else {
                *dest++ = '%';
                if (*format) *dest++ = *format;
            }
        } else {
            *dest++ = *format;
        }

        if (*format) format++;
    }

    *dest = '\0';
    va_end(args);
    return (int)(dest - str);
}
