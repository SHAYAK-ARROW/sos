#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

/*
 * keyboard_init()
 *
 * Initialises the keyboard subsystem.
 * Tries USB HID first; falls back to PS/2 if no USB keyboard is found.
 * Sets an internal flag indicating which backend is active.
 */
void keyboard_init(void);

/*
 * keyboard_getchar()
 *
 * Blocking read.  Returns the next ASCII character from whichever
 * keyboard backend (USB HID or PS/2) is active.
 */
char keyboard_getchar(void);

/*
 * keyboard_readline()
 *
 * Reads a newline-terminated line into buf (maxlen includes NUL).
 * Uses the active backend.
 */
void keyboard_readline(char *buf, int maxlen);

/*
 * keyboard_backend_name()
 *
 * Returns a string identifying the active backend:
 *   "USB HID"  — USB Human Interface Device keyboard
 *   "PS/2"     — legacy PS/2 keyboard controller
 */
const char *keyboard_backend_name(void);

#endif /* KEYBOARD_H */
