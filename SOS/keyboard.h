#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

typedef struct {
    uint8_t shift;
    uint8_t caps_lock;
    uint8_t ctrl;
    uint8_t alt;
} keyboard_state_t;

void keyboard_init(void);
char keyboard_get_char(void);
keyboard_state_t keyboard_get_state(void);

#endif
