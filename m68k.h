#ifndef _M68K_H
#define _M68K_H

#include <stdbool.h>
#include <stdint.h>
#include "mem.h"

typedef void (*m68k_trap_hook_t)(uint32_t d[8]);

typedef enum {
  M68K_LOCATION_NONE, /* Not Set */
  M68K_LOCATION_DR,   /* Data Register */
  M68K_LOCATION_AR,   /* Address Register */
  M68K_LOCATION_MEM,  /* Memory */
  M68K_LOCATION_IMM,  /* Immediate */
} m68k_location_t;

typedef struct m68k_ea_s {
  m68k_location_t l;
  uint32_t n;
  bool program_space;
} m68k_ea_t;

typedef struct m68k_s {
  uint32_t pc; /* Program Counter */
  uint32_t d[8]; /* Data Registers */
  uint32_t a[8]; /* Address Registers */
  uint32_t ssp; /* Supervisor Stack Pointer */

  union {
    struct {
      uint16_t c  : 1; /* Carry */
      uint16_t v  : 1; /* Overflow */
      uint16_t z  : 1; /* Zero */
      uint16_t n  : 1; /* Negative */
      uint16_t x  : 1; /* Extend */
      uint16_t u1 : 1; /* Unused #1 */
      uint16_t u2 : 1; /* Unused #2 */
      uint16_t u3 : 1; /* Unused #3 */
      uint16_t i0 : 1; /* Interrupt Priorty Mask 0 */
      uint16_t i1 : 1; /* Interrupt Priorty Mask 1 */
      uint16_t i2 : 1; /* Interrupt Priorty Mask 2 */
      uint16_t u4 : 1; /* Unused #4 */
      uint16_t m  : 1; /* Master/Interrupt State */
      uint16_t s  : 1; /* Supervisor/User State */
      uint16_t t0 : 1; /* Trace Enable 0 */
      uint16_t t1 : 1; /* Trace Enable 1 */
    } status;
    uint16_t sr; /* Status Register */
  };

  uint32_t old_pc; /* Old Program Counter */
  uint16_t opcode; /* Current Opcode */
  m68k_ea_t src;   /* Current Source */
  m68k_ea_t dst;   /* Current Destination */

  m68k_trap_hook_t trap_15_hook;
} m68k_t;

#define M68K_SP 7 /* User Stack Pointer = A7 */

#define M68K_VECTOR_ADDRESS_ERROR               0x0000000C
#define M68K_VECTOR_ILLEGAL_INSTRUCTION         0x00000010
#define M68K_VECTOR_DIVIDE_BY_ZERO              0x00000014
#define M68K_VECTOR_CHK_INSTRUCTION             0x00000018
#define M68K_VECTOR_TRAPV_INSTRUCTION           0x0000001C
#define M68K_VECTOR_PRIVILEGE_VIOLATION         0x00000020
#define M68K_VECTOR_UNIMPLEMENTED_A_LINE_OPCODE 0x00000028
#define M68K_VECTOR_UNIMPLEMENTED_F_LINE_OPCODE 0x0000002C

void m68k_execute(m68k_t *cpu, mem_t *mem);
void m68k_init(m68k_t *cpu);

#endif /* _M68K_H */
