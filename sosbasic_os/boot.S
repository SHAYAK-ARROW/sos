/* SOSBasic OS - boot.S (Multiboot Entry) */
.intel_syntax noprefix

.global _start
.extern kernel_main

.set MULTIBOOT_PAGE_ALIGN, 1 << 0
.set MULTIBOOT_MEMORY_INFO, 1 << 1
.set MULTIBOOT_HEADER_FLAGS, MULTIBOOT_PAGE_ALIGN | MULTIBOOT_MEMORY_INFO
.set MULTIBOOT_HEADER_MAGIC, 0x1BADB002
.set MULTIBOOT_CHECKSUM, -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)

.section .multiboot
.align 4
.long MULTIBOOT_HEADER_MAGIC
.long MULTIBOOT_HEADER_FLAGS
.long MULTIBOOT_CHECKSUM

.section .bss
.align 16
stack_bottom:
.skip 16384 /* 16 KB Stack */
stack_top:

.section .text
_start:
    mov esp, offset stack_top
    push ebx
    push eax
    call kernel_main

cli_hlt_loop:
    cli
    hlt
    jmp cli_hlt_loop

.section .note.GNU-stack,"",@progbits
