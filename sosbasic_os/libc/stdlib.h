#ifndef LIBC_STDLIB_H
#define LIBC_STDLIB_H

#include <stddef.h>
#include <stdint.h>

void *malloc(size_t size);
void free(void *ptr);
int atoi(const char *str);
char *itoa(int value, char *str, int base);
int abs(int n);

#endif
