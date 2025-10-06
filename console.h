#ifndef _CONSOLE_H
#define _CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

uint8_t console_status(void);
uint8_t console_read(void);
void console_write(uint8_t value);

bool console_warp_mode_toggle(void);
void console_inject(uint8_t value);
int console_inject_file(const char *filename);
void console_pause(void);
void console_resume(void);
void console_init(void);

#endif /* _CONSOLE_H */
