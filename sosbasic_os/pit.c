#include "pit.h"
#include "io.h"

static volatile uint32_t pit_ticks = 0;

void pit_init(uint32_t frequency_hz) {
    uint16_t divisor = (uint16_t)(PIT_FREQUENCY / frequency_hz);
    // Channel 0, lobyte/hibyte, Mode 2 (Rate Generator), Binary
    outb(0x43, 0x34);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

// Hardware-calibrated millisecond delay using PIT Channel 2 counter
void pit_delay_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        // PIT Channel 2 1ms countdown loop
        // 1 ms = 1193 ticks of 1.193182 MHz clock
        outb(0x43, 0xB0); // Channel 2, lobyte/hibyte, Mode 0
        outb(0x42, 1193 & 0xFF);
        outb(0x42, (1193 >> 8) & 0xFF);

        // Enable speaker gate to start channel 2
        uint8_t val = inb(0x61);
        outb(0x61, val | 0x01);

        // Wait until OUT bit 5 of port 0x61 becomes high (countdown complete)
        while ((inb(0x61) & 0x20) == 0);

        outb(0x61, val & ~0x01); // Disable gate
    }
}

uint32_t pit_get_ticks(void) {
    return pit_ticks;
}
