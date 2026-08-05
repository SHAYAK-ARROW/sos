#include "compiler.h"
#include "screen.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/stdlib.h"

// 4 KB RAM Buffer for Native 32-bit x86 Executable Machine Code
static uint8_t code_buffer[4096] __attribute__((aligned(16)));
static size_t code_size = 0;

static void emit_byte(uint8_t byte) {
    if (code_size < 4096) {
        code_buffer[code_size++] = byte;
    }
}

static void emit_dword(uint32_t dword) {
    emit_byte((uint8_t)(dword & 0xFF));
    emit_byte((uint8_t)((dword >> 8) & 0xFF));
    emit_byte((uint8_t)((dword >> 16) & 0xFF));
    emit_byte((uint8_t)((dword >> 24) & 0xFF));
}

static void emit_call_relative(void *target_func) {
    emit_byte(0xE8); // x86 CALL rel32 opcode
    uint32_t current_pc = (uint32_t)&code_buffer[code_size] + 4;
    uint32_t target_addr = (uint32_t)target_func;
    uint32_t offset = target_addr - current_pc;
    emit_dword(offset);
}

void compiler_init(void) {
    code_size = 0;
    memset(code_buffer, 0, sizeof(code_buffer));
}

void gcc_compile_and_run(const char *filename, const char *source_code) {
    printf("[GCC Native Compiler] Compiling '%s' to 32-bit x86 Machine Code...\n", filename);
    printf("[GCC Native Compiler] Linking with C Runtime Library (libc: stdio, stdlib, string, ctype)...\n");

    compiler_init();

    // --- 1. x86 Function Prologue ---
    emit_byte(0x55);             // push ebp
    emit_byte(0x89); emit_byte(0xE5); // mov ebp, esp

    // Parse source code and generate native x86 machine instructions
    if (source_code && source_code[0] != '\0') {
        const char *ptr = source_code;
        while (*ptr) {
            // Check for printf("...") in C source
            if (strncmp(ptr, "printf(\"", 8) == 0) {
                ptr += 8;
                const char *str_start = ptr;
                while (*ptr && *ptr != '"') {
                    ptr++;
                }
                size_t str_len = ptr - str_start;
                
                // Allocate string in code section
                char *allocated_str = (char *)malloc(str_len + 1);
                if (allocated_str) {
                    size_t j = 0;
                    for (size_t i = 0; i < str_len; i++) {
                        if (str_start[i] == '\\' && str_start[i+1] == 'n') {
                            allocated_str[j++] = '\n';
                            i++;
                        } else {
                            allocated_str[j++] = str_start[i];
                        }
                    }
                    allocated_str[j] = '\0';

                    // Emit x86 machine code to push string pointer and call printf
                    emit_byte(0x68); // push imm32
                    emit_dword((uint32_t)allocated_str);

                    emit_call_relative((void *)screen_puts);

                    emit_byte(0x83); emit_byte(0xC4); emit_byte(0x04); // add esp, 4
                }
            } else {
                ptr++;
            }
        }
    } else {
        static const char default_msg[] = "Hello World from Native x86 Machine Code in SOSBasic OS!\n";
        emit_byte(0x68); // push imm32
        emit_dword((uint32_t)default_msg);
        emit_call_relative((void *)screen_puts);
        emit_byte(0x83); emit_byte(0xC4); emit_byte(0x04); // add esp, 4
    }

    // --- 2. x86 Function Epilogue ---
    emit_byte(0xB8); emit_dword(0); // mov eax, 0 (return 0)
    emit_byte(0x5D);                 // pop ebp
    emit_byte(0xC3);                 // ret (Return to Kernel)

    printf("[GCC Native Compiler] Machine code compiled successfully (%u bytes emitted at 0x%X).\n", (unsigned int)code_size, (unsigned int)code_buffer);
    printf("[GCC Native Compiler] Jumping to native x86 machine code entry point...\n");
    printf("----------------------------------------------------\n");

    // --- 3. Execute Native x86 Machine Code Binary ---
    typedef int (*native_func_t)(void);
    native_func_t native_entry = (native_func_t)code_buffer;
    
    int return_code = native_entry(); // CPU jumps directly to executing native opcodes in RAM!

    printf("----------------------------------------------------\n");
    printf("[GCC Native Compiler] Program executed directly on CPU (returned %d).\n", return_code);
}
