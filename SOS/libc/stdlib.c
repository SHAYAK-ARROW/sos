#include "stdlib.h"

#define HEAP_SIZE 1048576 // 1 MB Bare-Metal Heap
static uint8_t heap_memory[HEAP_SIZE];
static size_t heap_offset = 0;

void *malloc(size_t size) {
    if (heap_offset + size > HEAP_SIZE) {
        return NULL; // Out of memory
    }
    void *ptr = &heap_memory[heap_offset];
    heap_offset += (size + 7) & ~7; // Align to 8 bytes
    return ptr;
}

void free(void *ptr) {
    (void)ptr; // Bump allocator free
}

int atoi(const char *str) {
    int res = 0;
    int sign = 1;
    if (*str == '-') {
        sign = -1;
        str++;
    }
    while (*str >= '0' && *str <= '9') {
        res = res * 10 + (*str - '0');
        str++;
    }
    return res * sign;
}

char *itoa(int value, char *str, int base) {
    char *rc;
    char *ptr;
    char *low;
    if (base < 2 || base > 36) {
        *str = '\0';
        return str;
    }
    rc = ptr = str;
    if (value < 0 && base == 10) {
        *ptr++ = '-';
    }
    low = ptr;
    do {
        *ptr++ = "0123456789abcdefghijklmnopqrstuvwxyz"[abs(value % base)];
        value /= base;
    } while (value);
    *ptr-- = '\0';
    while (low < ptr) {
        char tmp = *low;
        *low++ = *ptr;
        *ptr-- = tmp;
    }
    return rc;
}

int abs(int n) {
    return (n < 0) ? -n : n;
}
