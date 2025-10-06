#ifndef _M68K_TRACE_H
#define _M68K_TRACE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "m68k.h"

void m68k_trace_start(m68k_t *cpu);
void m68k_trace_mc(uint16_t mc);
void m68k_trace_op_mnemonic(const char *s);
void m68k_trace_op_src(const char *format, ...);
void m68k_trace_op_dst(const char *format, ...);
void m68k_trace_end(void);

void m68k_trace_init(void);
void m68k_trace_dump(FILE *fh, bool compact);

#endif /* _M68K_TRACE_H */
