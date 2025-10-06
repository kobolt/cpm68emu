#ifndef _DEBUGGER_H
#define _DEBUGGER_H

#include <stdbool.h>
#include <stdint.h>
#include "m68k.h"
#include "mem.h"
#include "ramdisk.h"

bool debugger(m68k_t *cpu, mem_t *mem, ramdisk_t *ramdisk);
#ifdef CPU_BREAKPOINT
extern int32_t debugger_breakpoint_pc;
#endif /* CPU_BREAKPOINT */

#endif /* _DEBUGGER_H */
