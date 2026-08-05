#ifndef COMPILER_H
#define COMPILER_H

#include <stdint.h>

void compiler_init(void);
void gcc_compile_and_run(const char *filename, const char *source_code);

#endif
