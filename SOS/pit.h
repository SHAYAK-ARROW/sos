#ifndef PIT_H
#define PIT_H

#include <stdint.h>

#define PIT_FREQUENCY 1193182 // 1.193182 MHz Base Frequency

void pit_init(uint32_t frequency_hz);
void pit_delay_ms(uint32_t ms);
uint32_t pit_get_ticks(void);

#endif
