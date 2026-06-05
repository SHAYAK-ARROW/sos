#include <stdint.h>
#include "idt.h"

/*
 * idt.c — Interrupt Descriptor Table
 *
 * FIX 1: naked ISR handler গুলো ভুল ছিল।
 *   - isr_err তে শুধু pop+hlt ছিল — stack corrupt হত।
 *   - isr_noerr তে সরাসরি hlt — registers save হত না।
 *   সব handler এ pushal/popal দিয়ে সব register save করা হচ্ছে।
 *   Error code আছে এমন exception এ আলাদা handler।
 *
 * FIX 2: irq_default এ শুধু master PIC (0x20) কে EOI পাঠানো হচ্ছিল।
 *   IRQ8-15 (slave PIC) এর জন্য slave (0xA0) তেও EOI দরকার।
 *   IRQ number না জানলে দুটোতেই EOI দিলে safe।
 */

struct idt_entry idt[256];
struct idt_ptr   idtp;

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low  =  base        & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel       = sel;
    idt[num].always0   = 0;
    idt[num].flags     = flags;
}

/*
 * Exception handler — error code নেই (INT 0,1,2,3,4,5,6,7,9,16,17,18,19)
 * Stack layout on entry: [EIP] [CS] [EFLAGS]
 */
__attribute__((naked)) static void isr_noerr(void) {
    __asm__ volatile(
        "cli\n"
        "pushal\n"
        /* এখানে serial/terminal debug add করা যাবে */
        "popal\n"
        "iret\n"
    );
}

/*
 * Exception handler — error code আছে (INT 8,10,11,12,13,14)
 * Stack layout on entry: [ERROR_CODE] [EIP] [CS] [EFLAGS]
 * Error code টা pop করে সরিয়ে দিতে হবে, নাহলে iret ভুল address এ যাবে।
 */
__attribute__((naked)) static void isr_err(void) {
    __asm__ volatile(
        "cli\n"
        "pushal\n"
        /* ESP+32 তে error code আছে — আপাতত ignore করছি */
        "popal\n"
        "add $4, %esp\n"   /* error code discard */
        "iret\n"
    );
}

/*
 * Hardware IRQ default handler (IRQ0–IRQ15)
 * FIX 2: master + slave PIC দুটোতেই EOI।
 * IRQ8+ এর জন্য slave PIC (0xA0) কেও EOI দরকার।
 */
__attribute__((naked)) static void irq_default(void) {
    __asm__ volatile(
        "pushal\n"
        /* Slave PIC EOI (IRQ8-15 এর জন্য, master এর আগে) */
        "mov $0x20, %al\n"
        "out %al, $0xA0\n"   /* slave PIC EOI */
        "out %al, $0x20\n"   /* master PIC EOI */
        "popal\n"
        "iret\n"
    );
}

void idt_init(void) {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base  = (uint32_t)(uintptr_t)&idt;

    /* সব entry আগে default দিয়ে ভরো */
    for (int i = 0;  i < 32;  i++)
        idt_set_gate((uint8_t)i, (uint32_t)(uintptr_t)isr_noerr, 0x08, 0x8E);
    for (int i = 32; i < 256; i++)
        idt_set_gate((uint8_t)i, (uint32_t)(uintptr_t)irq_default, 0x08, 0x8E);

    /* Error code আছে এমন exceptions override করো */
    idt_set_gate( 8, (uint32_t)(uintptr_t)isr_err, 0x08, 0x8E); /* Double Fault    */
    idt_set_gate(10, (uint32_t)(uintptr_t)isr_err, 0x08, 0x8E); /* Invalid TSS     */
    idt_set_gate(11, (uint32_t)(uintptr_t)isr_err, 0x08, 0x8E); /* Segment NP      */
    idt_set_gate(12, (uint32_t)(uintptr_t)isr_err, 0x08, 0x8E); /* Stack Fault     */
    idt_set_gate(13, (uint32_t)(uintptr_t)isr_err, 0x08, 0x8E); /* GPF             */
    idt_set_gate(14, (uint32_t)(uintptr_t)isr_err, 0x08, 0x8E); /* Page Fault      */

    __asm__ volatile("lidt (%0)" : : "r"(&idtp));
}
