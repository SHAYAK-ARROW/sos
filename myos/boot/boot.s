/* boot.s - Multiboot entry point */

.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot, "a"
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

.section .bss
.align 16
stack_bottom:
.skip 16384
stack_top:

.section .text
.global _start
.type   _start, @function

_start:
    /* প্রথমে ইবিএক্স (multiboot_info) এবং ইএএক্স (magic) অন্য রেজিস্টারে সেভ করো */
    mov %eax, %edx      /* ম্যাজিক নাম্বার EDX-এ সেভ করলাম */
    mov %ebx, %esi      /* মবডি (MBD) পয়েণ্টার ESI-তে সেভ করলাম */

    /* এবার .bss সেকশন জিরো করো */
    mov $_bss_start, %edi
    mov $_bss_end, %ecx
    sub %edi, %ecx
    xor %eax, %eax
    rep stosb

    /* স্ট্যাক সেটআপ করো */
    mov $stack_top, %esp

    /* সেভ করা ভ্যালুগুলো ফেরত আনো */
    push %edx           /* ম্যাজিক নাম্বার পুশ করো */
    push %esi           /* MBD পুশ করো */
    
    call kernel_main

1:  hlt
    jmp 1b
.size _start, . - _start