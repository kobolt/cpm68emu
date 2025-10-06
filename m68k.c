#include "m68k.h"
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m68k_trace.h"
#include "mem.h"
#include "panic.h"



#ifndef CPU_TRACE
#define m68k_trace_start(...)
#define m68k_trace_mc(...)
#define m68k_trace_op_mnemonic(...)
#define m68k_trace_op_src(...)
#define m68k_trace_op_dst(...)
#define m68k_trace_end(...)
#endif

#define EA_MODE_DR_DIRECT        0b000 /* Dn */
#define EA_MODE_AR_DIRECT        0b001 /* An */
#define EA_MODE_AR_INDIRECT      0b010 /* (An) */
#define EA_MODE_AR_POST_INC      0b011 /* (An)+ */
#define EA_MODE_AR_PRE_DEC       0b100 /* -(An) */
#define EA_MODE_AR_DISP_16       0b101 /* (d16,An) */
#define EA_MODE_AR_DISP_8        0b110 /* (d8,An,Xn) */
#define EA_MODE_EXT              0b111
#define EA_MODE_EXT_ABS_WORD     0b000 /* (xxx).W */
#define EA_MODE_EXT_ABS_LONG     0b001 /* (xxx).L */
#define EA_MODE_EXT_PC_DISP_16   0b010 /* (d16,PC) */
#define EA_MODE_EXT_PC_DISP_8    0b011 /* (d8,PC,Xn) */
#define EA_MODE_EXT_IMMEDIATE    0b100 /* #<data> */

static jmp_buf m68k_exception_jmp;



static inline uint16_t m68k_sr_filter_bits(uint16_t value)
{
  /* Filter Bits:  10SM-210---XNZVC */
  return value & 0b1010011100011111;
}



static inline uint32_t m68k_address_reg_value(m68k_t *cpu, uint8_t reg)
{
  if (reg == M68K_SP && cpu->status.s == 1) {
    return cpu->ssp;
  } else {
    return cpu->a[reg];
  }
}



static inline uint32_t m68k_ext_word_reg_value(m68k_t *cpu, uint16_t ext_word)
{
  if (ext_word >> 15) { /* Address Register */
    if ((ext_word >> 11) & 1) { /* Long Word */
      return m68k_address_reg_value(cpu, (ext_word >> 12) & 0b111);
    } else { /* Sign-Extended Word */
      return (int16_t)(m68k_address_reg_value(cpu,
        (ext_word >> 12) & 0b111) & 0xFFFF);
    }
  } else { /* Data Register */
    if ((ext_word >> 11) & 1) { /* Long Word */
      return cpu->d[(ext_word >> 12) & 0b111];
    } else { /* Sign-Extended Word */
      return (int16_t)(cpu->d[(ext_word >> 12) & 0b111] & 0xFFFF);
    }
  }
}



static inline void m68k_address_reg_set_word(m68k_t *cpu, uint8_t reg,
  uint16_t value)
{
  if (reg == M68K_SP && cpu->status.s == 1) {
    cpu->ssp &= ~0xFFFF;
    cpu->ssp |= value;
  } else {
    cpu->a[reg] &= ~0xFFFF;
    cpu->a[reg] |= value;
  }
}



static inline void m68k_address_reg_set_long(m68k_t *cpu, uint8_t reg,
  uint32_t value)
{
  if (reg == M68K_SP && cpu->status.s == 1) {
    cpu->ssp = value;
  } else {
    cpu->a[reg] = value;
  }
}



static inline void m68k_address_reg_inc(m68k_t *cpu, uint8_t reg, int width)
{
  if (reg == M68K_SP) {
    if (width < 2) {
      width = 2;
    }
    if (cpu->status.s == 1) {
      cpu->ssp += width;
    } else {
      cpu->a[reg] += width;
    }
  } else {
    cpu->a[reg] += width;
  }
}



static inline void m68k_address_reg_dec(m68k_t *cpu, uint8_t reg, int width)
{
  if (reg == M68K_SP) {
    if (width < 2) {
      width = 2;
    }
    if (cpu->status.s == 1) {
      cpu->ssp -= width;
    } else {
      cpu->a[reg] -= width;
    }
  } else {
    cpu->a[reg] -= width;
  }
}



static inline uint16_t m68k_fetch(m68k_t *cpu, mem_t *mem)
{
  bool error = false;
  cpu->opcode = mem_read_word(mem, cpu->pc, &error);
  cpu->pc += 2;
  if (cpu->pc > 0xFFFFFF) {
    panic("Program Counter Overflow!\n");
  }
  m68k_trace_mc(cpu->opcode);
  return cpu->opcode;
}



static inline uint16_t m68k_ssp_pop(m68k_t *cpu, mem_t *mem)
{
  uint16_t value;
  bool error = false;
  value = mem_read_word(mem, cpu->ssp, &error);
  cpu->ssp += 2;
  return value;
}

static inline uint16_t m68k_usp_pop(m68k_t *cpu, mem_t *mem)
{
  uint16_t value;
  bool error = false;
  value = mem_read_word(mem, cpu->a[M68K_SP], &error);
  cpu->a[M68K_SP] += 2;
  return value;
}

static inline uint16_t m68k_stack_pop(m68k_t *cpu, mem_t *mem)
{
  if (cpu->status.s) {
    return m68k_ssp_pop(cpu, mem);
  } else {
    return m68k_usp_pop(cpu, mem);
  }
}



static inline void m68k_ssp_push(m68k_t *cpu, mem_t *mem, uint16_t value)
{
  bool error = false;
  cpu->ssp -= 2;
  mem_write_word(mem, cpu->ssp, value, &error);
}

static inline void m68k_usp_push(m68k_t *cpu, mem_t *mem, uint16_t value)
{
  bool error = false;
  cpu->a[M68K_SP] -= 2;
  mem_write_word(mem, cpu->a[M68K_SP], value, &error);
}

static inline void m68k_stack_push(m68k_t *cpu, mem_t *mem, uint16_t value)
{
  if (cpu->status.s) {
    m68k_ssp_push(cpu, mem, value);
  } else {
    m68k_usp_push(cpu, mem, value);
  }
}



static inline void m68k_address_error(m68k_t *cpu, mem_t *mem,
  uint32_t address, bool read, bool program_space)
{
  bool error = false;
  uint16_t value;

  m68k_ssp_push(cpu, mem, cpu->pc % 0x10000);
  m68k_ssp_push(cpu, mem, cpu->pc / 0x10000);
  m68k_ssp_push(cpu, mem, cpu->sr);
  m68k_ssp_push(cpu, mem, cpu->opcode);
  m68k_ssp_push(cpu, mem, address % 0x10000);
  m68k_ssp_push(cpu, mem, address / 0x10000);
  value = cpu->opcode & ~0b11111;
  if (read) {
    value |= 0b10000; /* Read (instead of Write) */
  }
  if (cpu->status.s) {
    if (program_space) {
      value |= 0b110; /* Supervisor Program */
    } else {
      value |= 0b101; /* Supervisor Data */
    }
  } else {
    if (program_space) {
      value |= 0b010; /* User Program */
    } else {
      value |= 0b001; /* User Data */
    }
  }
  m68k_ssp_push(cpu, mem, value);
  cpu->pc = mem_read_long(mem, M68K_VECTOR_ADDRESS_ERROR, &error);
  cpu->sr &= ~0x8000; /* Clear Trace Bit */
  cpu->sr |= 0x2000; /* Set Supervisor Bit */

  longjmp(m68k_exception_jmp, 1);
}



static inline void m68k_exception(m68k_t *cpu, mem_t *mem, uint32_t vector)
{
  bool error = false;

  cpu->pc = cpu->old_pc;
  m68k_ssp_push(cpu, mem, cpu->pc % 0x10000);
  m68k_ssp_push(cpu, mem, cpu->pc / 0x10000);
  m68k_ssp_push(cpu, mem, cpu->sr);
  cpu->pc = mem_read_long(mem, vector, &error);
  cpu->sr &= ~0x8000; /* Clear Trace Bit */
  cpu->sr |= 0x2000; /* Set Supervisor Bit */

  longjmp(m68k_exception_jmp, 1);
}



static uint8_t m68k_add_byte(m68k_t *cpu, uint8_t in1, uint8_t in2)
{
  uint8_t result = in1 + in2;
  cpu->status.n = result >> 7;
  cpu->status.z = result == 0;
  cpu->status.c = ((uint16_t)(in1 + in2) & 0x100) > 0;
  cpu->status.v = (((in1 & 0x80) && (in2 & 0x80) && !(result & 0x80)) ||
                  (!(in1 & 0x80) && !(in2 & 0x80) && (result & 0x80)));
  cpu->status.x = cpu->status.c;
  return result;
}

static uint16_t m68k_add_word(m68k_t *cpu, uint16_t in1, uint16_t in2,
  bool skip_cc_set)
{
  uint16_t result = in1 + in2;
  if (skip_cc_set) {
    return result;
  }
  cpu->status.n = result >> 15;
  cpu->status.z = result == 0;
  cpu->status.c = ((uint32_t)(in1 + in2) & 0x10000) > 0;
  cpu->status.v = (((in1 & 0x8000) && (in2 & 0x8000) && !(result & 0x8000)) ||
                  (!(in1 & 0x8000) && !(in2 & 0x8000) && (result & 0x8000)));
  cpu->status.x = cpu->status.c;
  return result;
}

static uint32_t m68k_add_long(m68k_t *cpu, uint32_t in1, uint32_t in2,
  bool skip_cc_set)
{
  uint32_t result = in1 + in2;
  if (skip_cc_set) {
    return result;
  }
  cpu->status.n = result >> 31;
  cpu->status.z = result == 0;
  cpu->status.c = (((uint64_t)in1 + (uint64_t)in2) & 0x100000000) > 0;
  cpu->status.v = (((in1 & 0x80000000) &&
                    (in2 & 0x80000000) &&
                !(result & 0x80000000)) ||
                  (!(in1 & 0x80000000) &&
                   !(in2 & 0x80000000) &&
                 (result & 0x80000000)));
  cpu->status.x = cpu->status.c;
  return result;
}



static uint8_t m68k_addx_byte(m68k_t *cpu, uint8_t in1, uint8_t in2)
{
  uint8_t result = in1 + in2 + cpu->status.x;
  cpu->status.n = result >> 7;
  if (result != 0) {
    cpu->status.z = 0;
  }
  cpu->status.c = ((uint16_t)(in1 + in2 + cpu->status.x) & 0x100) > 0;
  cpu->status.v = (((in1 & 0x80) && (in2 & 0x80) && !(result & 0x80)) ||
                  (!(in1 & 0x80) && !(in2 & 0x80) && (result & 0x80)));
  cpu->status.x = cpu->status.c;
  return result;
}

static uint16_t m68k_addx_word(m68k_t *cpu, uint16_t in1, uint16_t in2)
{
  uint16_t result = in1 + in2 + cpu->status.x;
  cpu->status.n = result >> 15;
  if (result != 0) {
    cpu->status.z = 0;
  }
  cpu->status.c = ((uint32_t)(in1 + in2 + cpu->status.x) & 0x10000) > 0;
  cpu->status.v = (((in1 & 0x8000) && (in2 & 0x8000) && !(result & 0x8000)) ||
                  (!(in1 & 0x8000) && !(in2 & 0x8000) && (result & 0x8000)));
  cpu->status.x = cpu->status.c;
  return result;
}

static uint32_t m68k_addx_long(m68k_t *cpu, uint32_t in1, uint32_t in2)
{
  uint32_t result = in1 + in2 + cpu->status.x;
  cpu->status.n = result >> 31;
  if (result != 0) {
    cpu->status.z = 0;
  }
  cpu->status.c = (((uint64_t)in1 + (uint64_t)in2 + cpu->status.x) &
                   0x100000000) > 0;
  cpu->status.v = (((in1 & 0x80000000) &&
                    (in2 & 0x80000000) &&
                !(result & 0x80000000)) ||
                  (!(in1 & 0x80000000) &&
                   !(in2 & 0x80000000) &&
                 (result & 0x80000000)));
  cpu->status.x = cpu->status.c;
  return result;
}



static uint8_t m68k_add_bcd(m68k_t *cpu, uint8_t in1, uint8_t in2)
{
  uint16_t result = (in1 & 0x0F) + (in2 & 0x0F) + cpu->status.x;
  if (result > 9) {
    result += 6;
  }
  result += (in1 & 0xF0) + (in2 & 0xF0);
  if (result > 0x9F) {
    result -= 0xA0;
    cpu->status.c = 1;
  } else {
    cpu->status.c = 0;
  }
  result &= 0xFF;
  if (result != 0) {
    cpu->status.z = 0;
  }
  cpu->status.x = cpu->status.c;
  return result;
}



static uint8_t m68k_and_byte(m68k_t *cpu, uint8_t in1, uint8_t in2)
{
  uint8_t result = in1 & in2;
  cpu->status.n = result >> 7;
  cpu->status.z = result == 0;
  cpu->status.c = 0;
  cpu->status.v = 0;
  return result;
}

static uint16_t m68k_and_word(m68k_t *cpu, uint16_t in1, uint16_t in2)
{
  uint16_t result = in1 & in2;
  cpu->status.n = result >> 15;
  cpu->status.z = result == 0;
  cpu->status.c = 0;
  cpu->status.v = 0;
  return result;
}

static uint32_t m68k_and_long(m68k_t *cpu, uint32_t in1, uint32_t in2)
{
  uint32_t result = in1 & in2;
  cpu->status.n = result >> 31;
  cpu->status.z = result == 0;
  cpu->status.c = 0;
  cpu->status.v = 0;
  return result;
}



static uint8_t m68k_asl_byte(m68k_t *cpu, uint8_t input, uint8_t count)
{
  bool msb = input >> 7;
  cpu->status.v = 0;
  if (count == 0) {
    cpu->status.c = 0;
  } else {
    while (count > 0) {
      cpu->status.c = input >> 7;
      input <<= 1;
      if (input >> 7 != msb) {
        cpu->status.v = 1;
      }
      count--;
    }
    cpu->status.x = cpu->status.c;
  }
  cpu->status.n = input >> 7;
  cpu->status.z = input == 0;
  return input;
}

static uint16_t m68k_asl_word(m68k_t *cpu, uint16_t input, uint8_t count)
{
  bool msb = input >> 15;
  cpu->status.v = 0;
  if (count == 0) {
    cpu->status.c = 0;
  } else {
    while (count > 0) {
      cpu->status.c = input >> 15;
      input <<= 1;
      if (input >> 15 != msb) {
        cpu->status.v = 1;
      }
      count--;
    }
    cpu->status.x = cpu->status.c;
  }
  cpu->status.n = input >> 15;
  cpu->status.z = input == 0;
  return input;
}

static uint32_t m68k_asl_long(m68k_t *cpu, uint32_t input, uint8_t count)
{
  bool msb = input >> 31;
  cpu->status.v = 0;
  if (count == 0) {
    cpu->status.c = 0;
  } else {
    while (count > 0) {
      cpu->status.c = input >> 31;
      input <<= 1;
      count--;
      if (input >> 31 != msb) {
        cpu->status.v = 1;
      }
    }
    cpu->status.x = cpu->status.c;
  }
  cpu->status.n = input >> 31;
  cpu->status.z = input == 0;
  return input;
}



static uint8_t m68k_asr_byte(m68k_t *cpu, uint8_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = 0;
  } else {
    while (count > 0) {
      cpu->status.c = input & 1;
      input >>= 1;
      input |= ((input >> 6) & 1) << 7;
      count--;
    }
    cpu->status.x = cpu->status.c;
  }
  cpu->status.n = input >> 7;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}

static uint16_t m68k_asr_word(m68k_t *cpu, uint16_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = 0;
  } else {
    while (count > 0) {
      cpu->status.c = input & 1;
      input >>= 1;
      input |= ((input >> 14) & 1) << 15;
      count--;
    }
    cpu->status.x = cpu->status.c;
  }
  cpu->status.n = input >> 15;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}

static uint32_t m68k_asr_long(m68k_t *cpu, uint32_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = 0;
  } else {
    while (count > 0) {
      cpu->status.c = input & 1;
      input >>= 1;
      input |= ((input >> 30) & 1) << 31;
      count--;
    }
    cpu->status.x = cpu->status.c;
  }
  cpu->status.n = input >> 31;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}



static void m68k_cmp_byte(m68k_t *cpu, uint8_t sub, uint8_t min)
{
  uint8_t result = min - sub;
  cpu->status.n = result >> 7;
  cpu->status.z = result == 0;
  cpu->status.c = ((uint16_t)(min - sub) & 0x100) > 0;
  cpu->status.v = (((min & 0x80) && !(sub & 0x80) && !(result & 0x80)) ||
                  (!(min & 0x80) && (sub & 0x80) && (result & 0x80)));
}

static void m68k_cmp_word(m68k_t *cpu, uint16_t sub, uint16_t min)
{
  uint16_t result = min - sub;
  cpu->status.n = result >> 15;
  cpu->status.z = result == 0;
  cpu->status.c = ((uint32_t)(min - sub) & 0x10000) > 0;
  cpu->status.v = (((min & 0x8000) && !(sub & 0x8000) && !(result & 0x8000)) ||
                  (!(min & 0x8000) && (sub & 0x8000) && (result & 0x8000)));
}

static void m68k_cmp_long(m68k_t *cpu, uint32_t sub, uint32_t min)
{
  uint32_t result = min - sub;
  cpu->status.n = result >> 31;
  cpu->status.z = result == 0;
  cpu->status.c = (((uint64_t)min - (uint64_t)sub) & 0x100000000) > 0;
  cpu->status.v = (((min & 0x80000000) &&
                   !(sub & 0x80000000) &&
                !(result & 0x80000000)) ||
                  (!(min & 0x80000000) &&
                    (sub & 0x80000000) &&
                 (result & 0x80000000)));
}



static uint8_t m68k_eor_byte(m68k_t *cpu, uint8_t in1, uint8_t in2)
{
  uint8_t result = in1 ^ in2;
  cpu->status.n = result >> 7;
  cpu->status.z = result == 0;
  cpu->status.c = 0;
  cpu->status.v = 0;
  return result;
}

static uint16_t m68k_eor_word(m68k_t *cpu, uint16_t in1, uint16_t in2)
{
  uint16_t result = in1 ^ in2;
  cpu->status.n = result >> 15;
  cpu->status.z = result == 0;
  cpu->status.c = 0;
  cpu->status.v = 0;
  return result;
}

static uint32_t m68k_eor_long(m68k_t *cpu, uint32_t in1, uint32_t in2)
{
  uint32_t result = in1 ^ in2;
  cpu->status.n = result >> 31;
  cpu->status.z = result == 0;
  cpu->status.c = 0;
  cpu->status.v = 0;
  return result;
}



static uint8_t m68k_lsl_byte(m68k_t *cpu, uint8_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = 0;
  } else {
    while (count > 0) {
      cpu->status.c = input >> 7;
      input <<= 1;
      count--;
    }
    cpu->status.x = cpu->status.c;
  }
  cpu->status.n = input >> 7;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}

static uint16_t m68k_lsl_word(m68k_t *cpu, uint16_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = 0;
  } else {
    while (count > 0) {
      cpu->status.c = input >> 15;
      input <<= 1;
      count--;
    }
    cpu->status.x = cpu->status.c;
  }
  cpu->status.n = input >> 15;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}

static uint32_t m68k_lsl_long(m68k_t *cpu, uint32_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = 0;
  } else {
    while (count > 0) {
      cpu->status.c = input >> 31;
      input <<= 1;
      count--;
    }
    cpu->status.x = cpu->status.c;
  }
  cpu->status.n = input >> 31;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}



static uint8_t m68k_lsr_byte(m68k_t *cpu, uint8_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = 0;
  } else {
    while (count > 0) {
      cpu->status.c = input & 1;
      input >>= 1;
      count--;
    }
    cpu->status.x = cpu->status.c;
  }
  cpu->status.n = input >> 7;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}

static uint16_t m68k_lsr_word(m68k_t *cpu, uint16_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = 0;
  } else {
    while (count > 0) {
      cpu->status.c = input & 1;
      input >>= 1;
      count--;
    }
    cpu->status.x = cpu->status.c;
  }
  cpu->status.n = input >> 15;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}

static uint32_t m68k_lsr_long(m68k_t *cpu, uint32_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = 0;
  } else {
    while (count > 0) {
      cpu->status.c = input & 1;
      input >>= 1;
      count--;
    }
    cpu->status.x = cpu->status.c;
  }
  cpu->status.n = input >> 31;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}



static uint8_t m68k_not_byte(m68k_t *cpu, uint8_t input)
{
  input = ~input;
  cpu->status.n = input >> 7;
  cpu->status.z = input == 0;
  cpu->status.c = 0;
  cpu->status.v = 0;
  return input;
}

static uint16_t m68k_not_word(m68k_t *cpu, uint16_t input)
{
  input = ~input;
  cpu->status.n = input >> 15;
  cpu->status.z = input == 0;
  cpu->status.c = 0;
  cpu->status.v = 0;
  return input;
}

static uint32_t m68k_not_long(m68k_t *cpu, uint32_t input)
{
  input = ~input;
  cpu->status.n = input >> 31;
  cpu->status.z = input == 0;
  cpu->status.c = 0;
  cpu->status.v = 0;
  return input;
}



static uint8_t m68k_or_byte(m68k_t *cpu, uint8_t in1, uint8_t in2)
{
  uint8_t result = in1 | in2;
  cpu->status.n = result >> 7;
  cpu->status.z = result == 0;
  cpu->status.c = 0;
  cpu->status.v = 0;
  return result;
}

static uint16_t m68k_or_word(m68k_t *cpu, uint16_t in1, uint16_t in2)
{
  uint16_t result = in1 | in2;
  cpu->status.n = result >> 15;
  cpu->status.z = result == 0;
  cpu->status.c = 0;
  cpu->status.v = 0;
  return result;
}

static uint32_t m68k_or_long(m68k_t *cpu, uint32_t in1, uint32_t in2)
{
  uint32_t result = in1 | in2;
  cpu->status.n = result >> 31;
  cpu->status.z = result == 0;
  cpu->status.c = 0;
  cpu->status.v = 0;
  return result;
}



static uint8_t m68k_rol_byte(m68k_t *cpu, uint8_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = 0;
  } else {
    while (count > 0) {
      cpu->status.c = input >> 7;
      input <<= 1;
      input |= cpu->status.c;
      count--;
    }
  }
  cpu->status.n = input >> 7;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}

static uint16_t m68k_rol_word(m68k_t *cpu, uint16_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = 0;
  } else {
    while (count > 0) {
      cpu->status.c = input >> 15;
      input <<= 1;
      input |= cpu->status.c;
      count--;
    }
  }
  cpu->status.n = input >> 15;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}

static uint32_t m68k_rol_long(m68k_t *cpu, uint32_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = 0;
  } else {
    while (count > 0) {
      cpu->status.c = input >> 31;
      input <<= 1;
      input |= cpu->status.c;
      count--;
    }
  }
  cpu->status.n = input >> 31;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}



static uint8_t m68k_ror_byte(m68k_t *cpu, uint8_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = 0;
  } else {
    while (count > 0) {
      cpu->status.c = input & 1;
      input >>= 1;
      input |= (cpu->status.c << 7);
      count--;
    }
  }
  cpu->status.n = input >> 7;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}

static uint16_t m68k_ror_word(m68k_t *cpu, uint16_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = 0;
  } else {
    while (count > 0) {
      cpu->status.c = input & 1;
      input >>= 1;
      input |= (cpu->status.c << 15);
      count--;
    }
  }
  cpu->status.n = input >> 15;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}

static uint32_t m68k_ror_long(m68k_t *cpu, uint32_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = 0;
  } else {
    while (count > 0) {
      cpu->status.c = input & 1;
      input >>= 1;
      input |= (cpu->status.c << 31);
      count--;
    }
  }
  cpu->status.n = input >> 31;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}



static uint8_t m68k_roxl_byte(m68k_t *cpu, uint8_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = cpu->status.x;
  } else {
    while (count > 0) {
      cpu->status.c = input >> 7;
      input <<= 1;
      input |= cpu->status.x;
      cpu->status.x = cpu->status.c;
      count--;
    }
  }
  cpu->status.n = input >> 7;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}

static uint16_t m68k_roxl_word(m68k_t *cpu, uint16_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = cpu->status.x;
  } else {
    while (count > 0) {
      cpu->status.c = input >> 15;
      input <<= 1;
      input |= cpu->status.x;
      cpu->status.x = cpu->status.c;
      count--;
    }
  }
  cpu->status.n = input >> 15;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}

static uint32_t m68k_roxl_long(m68k_t *cpu, uint32_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = cpu->status.x;
  } else {
    while (count > 0) {
      cpu->status.c = input >> 31;
      input <<= 1;
      input |= cpu->status.x;
      cpu->status.x = cpu->status.c;
      count--;
    }
  }
  cpu->status.n = input >> 31;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}



static uint8_t m68k_roxr_byte(m68k_t *cpu, uint8_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = cpu->status.x;
  } else {
    while (count > 0) {
      cpu->status.c = input & 1;
      input >>= 1;
      input |= (cpu->status.x << 7);
      cpu->status.x = cpu->status.c;
      count--;
    }
  }
  cpu->status.n = input >> 7;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}

static uint16_t m68k_roxr_word(m68k_t *cpu, uint16_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = cpu->status.x;
  } else {
    while (count > 0) {
      cpu->status.c = input & 1;
      input >>= 1;
      input |= (cpu->status.x << 15);
      cpu->status.x = cpu->status.c;
      count--;
    }
  }
  cpu->status.n = input >> 15;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}

static uint32_t m68k_roxr_long(m68k_t *cpu, uint32_t input, uint8_t count)
{
  if (count == 0) {
    cpu->status.c = cpu->status.x;
  } else {
    while (count > 0) {
      cpu->status.c = input & 1;
      input >>= 1;
      input |= (cpu->status.x << 31);
      cpu->status.x = cpu->status.c;
      count--;
    }
  }
  cpu->status.n = input >> 31;
  cpu->status.z = input == 0;
  cpu->status.v = 0;
  return input;
}



static uint8_t m68k_sub_byte(m68k_t *cpu, uint8_t sub, uint8_t min)
{
  uint8_t result = min - sub;
  cpu->status.n = result >> 7;
  cpu->status.z = result == 0;
  cpu->status.c = ((uint16_t)(min - sub) & 0x100) > 0;
  cpu->status.v = (((min & 0x80) && !(sub & 0x80) && !(result & 0x80)) ||
                  (!(min & 0x80) && (sub & 0x80) && (result & 0x80)));
  cpu->status.x = cpu->status.c;
  return result;
}

static uint16_t m68k_sub_word(m68k_t *cpu, uint16_t sub, uint16_t min,
  bool skip_cc_set)
{
  uint16_t result = min - sub;
  if (skip_cc_set) {
    return result;
  }
  cpu->status.n = result >> 15;
  cpu->status.z = result == 0;
  cpu->status.c = ((uint32_t)(min - sub) & 0x10000) > 0;
  cpu->status.v = (((min & 0x8000) && !(sub & 0x8000) && !(result & 0x8000)) ||
                  (!(min & 0x8000) && (sub & 0x8000) && (result & 0x8000)));
  cpu->status.x = cpu->status.c;
  return result;
}

static uint32_t m68k_sub_long(m68k_t *cpu, uint32_t sub, uint32_t min,
  bool skip_cc_set)
{
  uint32_t result = min - sub;
  if (skip_cc_set) {
    return result;
  }
  cpu->status.n = result >> 31;
  cpu->status.z = result == 0;
  cpu->status.c = (((uint64_t)min - (uint64_t)sub) & 0x100000000) > 0;
  cpu->status.v = (((min & 0x80000000) &&
                   !(sub & 0x80000000) &&
                !(result & 0x80000000)) ||
                  (!(min & 0x80000000) &&
                    (sub & 0x80000000) &&
                 (result & 0x80000000)));
  cpu->status.x = cpu->status.c;
  return result;
}



static uint8_t m68k_neg_byte(m68k_t *cpu, uint8_t input)
{
  return m68k_sub_byte(cpu, input, 0);
}

static uint16_t m68k_neg_word(m68k_t *cpu, uint16_t input)
{
  return m68k_sub_word(cpu, input, 0, false);
}

static uint32_t m68k_neg_long(m68k_t *cpu, uint32_t input)
{
  return m68k_sub_long(cpu, input, 0, false);
}



static uint8_t m68k_subx_byte(m68k_t *cpu, uint8_t sub, uint8_t min)
{
  uint8_t result = (min - sub) - cpu->status.x;
  cpu->status.n = result >> 7;
  if (result != 0) {
    cpu->status.z = 0;
  }
  cpu->status.c = ((uint16_t)(min - sub - cpu->status.x) & 0x100) > 0;
  cpu->status.v = (((min & 0x80) && !(sub & 0x80) && !(result & 0x80)) ||
                  (!(min & 0x80) && (sub & 0x80) && (result & 0x80)));
  cpu->status.x = cpu->status.c;
  return result;
}

static uint16_t m68k_subx_word(m68k_t *cpu, uint16_t sub, uint16_t min)
{
  uint16_t result = (min - sub) - cpu->status.x;
  cpu->status.n = result >> 15;
  if (result != 0) {
    cpu->status.z = 0;
  }
  cpu->status.c = ((uint32_t)(min - sub - cpu->status.x) & 0x10000) > 0;
  cpu->status.v = (((min & 0x8000) && !(sub & 0x8000) && !(result & 0x8000)) ||
                  (!(min & 0x8000) && (sub & 0x8000) && (result & 0x8000)));
  cpu->status.x = cpu->status.c;
  return result;
}

static uint32_t m68k_subx_long(m68k_t *cpu, uint32_t sub, uint32_t min)
{
  uint32_t result = (min - sub) - cpu->status.x;
  cpu->status.n = result >> 31;
  if (result != 0) {
    cpu->status.z = 0;
  }
  cpu->status.c = (((uint64_t)min - (uint64_t)sub - cpu->status.x) &
                   0x100000000) > 0;
  cpu->status.v = (((min & 0x80000000) &&
                   !(sub & 0x80000000) &&
                !(result & 0x80000000)) ||
                  (!(min & 0x80000000) &&
                    (sub & 0x80000000) &&
                 (result & 0x80000000)));
  cpu->status.x = cpu->status.c;
  return result;
}



static uint8_t m68k_sub_bcd(m68k_t *cpu, uint8_t sub, uint8_t min)
{
  uint16_t result = ((min & 0x0F) - (sub & 0x0F)) - cpu->status.x;
  if (result > 0xF) {
    result += (min & 0xF0) - (sub & 0xF0);
    if (result > 0xFF) {
      result += 0xA0;
      cpu->status.c = 1;
    } else {
      if (result < 6) {
        cpu->status.c = 1;
      } else {
        cpu->status.c = 0;
      }
    }
    result -= 6;
  } else {
    result += (min & 0xF0) - (sub & 0xF0);
    if (result > 0xFF) {
      result += 0xA0;
      cpu->status.c = 1;
    } else {
      cpu->status.c = 0;
    }
  }
  result &= 0xFF;
  if (result != 0) {
    cpu->status.z = 0;
  }
  cpu->status.x = cpu->status.c;
  return result;
}



static void m68k_src_set(m68k_t *cpu, mem_t *mem,
  uint8_t reg, uint8_t mode, int width)
{
  uint16_t ext_word;
  uint32_t address;

  cpu->src.program_space = false;

  switch (mode) {
  case EA_MODE_DR_DIRECT: /* Dn */
    m68k_trace_op_src("D%d", reg);
    cpu->src.l = M68K_LOCATION_DR;
    cpu->src.n = reg;
    break;

  case EA_MODE_AR_DIRECT: /* An */
    m68k_trace_op_src("A%d", reg);
    cpu->src.l = M68K_LOCATION_AR;
    cpu->src.n = reg;
    break;

  case EA_MODE_AR_INDIRECT: /* (An) */
    m68k_trace_op_src("(A%d)", reg);
    address = m68k_address_reg_value(cpu, reg);
    cpu->src.l = M68K_LOCATION_MEM;
    cpu->src.n = address;
    break;

  case EA_MODE_AR_POST_INC: /* (An)+ */
    m68k_trace_op_src("(A%d)+", reg);
    address = m68k_address_reg_value(cpu, reg);
    cpu->src.l = M68K_LOCATION_MEM;
    cpu->src.n = address;
    m68k_address_reg_inc(cpu, reg, width);
    break;

  case EA_MODE_AR_PRE_DEC: /* -(An) */
    m68k_trace_op_src("-(A%d)", reg);
    m68k_address_reg_dec(cpu, reg, width);
    address = m68k_address_reg_value(cpu, reg);
    cpu->src.l = M68K_LOCATION_MEM;
    cpu->src.n = address;
    break;

  case EA_MODE_AR_DISP_16: /* (d16,An) */
    m68k_trace_op_src("(d16, A%d)", reg);
    ext_word = m68k_fetch(cpu, mem);
    address = m68k_address_reg_value(cpu, reg);
    address += (int16_t)(ext_word & 0xFFFF);
    cpu->src.l = M68K_LOCATION_MEM;
    cpu->src.n = address;
    break;

  case EA_MODE_AR_DISP_8: /* (d8,An,Xn) */
    m68k_trace_op_src("(d8, A%d, Xn)", reg);
    ext_word = m68k_fetch(cpu, mem);
    address = m68k_address_reg_value(cpu, reg);
    address += (int8_t)(ext_word & 0xFF);
    address += m68k_ext_word_reg_value(cpu, ext_word);
    cpu->src.l = M68K_LOCATION_MEM;
    cpu->src.n = address;
    break;

  case EA_MODE_EXT:
    switch (reg) {
    case EA_MODE_EXT_ABS_WORD: /* (xxx).W */
      address = (int16_t)m68k_fetch(cpu, mem);
      m68k_trace_op_src("($%08x).W", address);
      cpu->src.l = M68K_LOCATION_MEM;
      cpu->src.n = address;
      break;

    case EA_MODE_EXT_ABS_LONG: /* (xxx).L */
      address = (m68k_fetch(cpu, mem) << 16);
      address += m68k_fetch(cpu, mem);
      m68k_trace_op_src("($%08x).L", address);
      cpu->src.l = M68K_LOCATION_MEM;
      cpu->src.n = address;
      break;

    case EA_MODE_EXT_PC_DISP_16: /* (d16,PC) */
      m68k_trace_op_src("(d16, PC)");
      ext_word = m68k_fetch(cpu, mem);
      address = cpu->pc - 2;
      address += (int16_t)(ext_word & 0xFFFF);
      cpu->src.l = M68K_LOCATION_MEM;
      cpu->src.n = address;
      cpu->src.program_space = true;
      break;

    case EA_MODE_EXT_PC_DISP_8: /* (d8,PC,Xn) */
      m68k_trace_op_src("(d8, PC, Xn)");
      ext_word = m68k_fetch(cpu, mem);
      address = cpu->pc - 2;
      address += (int8_t)(ext_word & 0xFF);
      address += m68k_ext_word_reg_value(cpu, ext_word);
      cpu->src.l = M68K_LOCATION_MEM;
      cpu->src.n = address;
      cpu->src.program_space = true;
      break;

    case EA_MODE_EXT_IMMEDIATE: /* #<data> */
      cpu->src.l = M68K_LOCATION_IMM;
      if (width == 4) {
        ext_word = m68k_fetch(cpu, mem);
        cpu->src.n = (ext_word & 0xFFFF) << 16;
        ext_word = m68k_fetch(cpu, mem);
        cpu->src.n |= ext_word & 0xFFFF;
        m68k_trace_op_src("#$%08x", cpu->src.n);
      } else {
        ext_word = m68k_fetch(cpu, mem);
        cpu->src.n = ext_word;
        if (width == 2) {
          m68k_trace_op_src("#$%04x", cpu->src.n);
        } else {
          m68k_trace_op_src("#$%02x", cpu->src.n & 0xFF);
        }
      }
      break;

    default:
      /* Unhandled effective address source. */
      m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
      break;
    }
    break;
  }
}



static uint8_t m68k_src_read_byte(m68k_t *cpu, mem_t *mem)
{
  uint8_t value = 0;

  switch (cpu->src.l) {
  case M68K_LOCATION_DR:
    value = cpu->d[cpu->src.n] & 0xFF;
    break;

  case M68K_LOCATION_MEM:
    value = mem_read_byte(mem, cpu->src.n);
    break;

  case M68K_LOCATION_IMM:
    value = cpu->src.n & 0xFF;
    break;

  default:
    /* Unhandled source byte read. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }

  return value;
}



static uint16_t m68k_src_read_word(m68k_t *cpu, mem_t *mem)
{
  uint16_t value = 0;
  bool error = false;

  switch (cpu->src.l) {
  case M68K_LOCATION_DR:
    value = cpu->d[cpu->src.n] & 0xFFFF;
    break;

  case M68K_LOCATION_AR:
    value = m68k_address_reg_value(cpu, cpu->src.n) & 0xFFFF;
    break;

  case M68K_LOCATION_MEM:
    value = mem_read_word(mem, cpu->src.n, &error);
    if (error) {
      m68k_address_error(cpu, mem, cpu->src.n, true, cpu->src.program_space);
    }
    break;

  case M68K_LOCATION_IMM:
    value = cpu->src.n & 0xFFFF;
    break;

  default:
    /* Unhandled source word read. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }

  return value;
}



static uint32_t m68k_src_read_long(m68k_t *cpu, mem_t *mem)
{
  uint32_t value = 0;
  bool error = false;

  switch (cpu->src.l) {
  case M68K_LOCATION_DR:
    value = cpu->d[cpu->src.n];
    break;

  case M68K_LOCATION_AR:
    value = m68k_address_reg_value(cpu, cpu->src.n);
    break;

  case M68K_LOCATION_MEM:
    value = mem_read_long(mem, cpu->src.n, &error);
    if (error) {
      m68k_address_error(cpu, mem, cpu->src.n, true, cpu->src.program_space);
    }
    break;

  case M68K_LOCATION_IMM:
    value = cpu->src.n;
    break;

  default:
    /* Unhandled source long read. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }

  return value;
}



static void m68k_dst_set(m68k_t *cpu, mem_t *mem,
  uint8_t reg, uint8_t mode, int width)
{
  uint16_t ext_word;
  uint32_t address;

  cpu->dst.program_space = false;

  switch (mode) {
  case EA_MODE_DR_DIRECT: /* Dn */
    m68k_trace_op_dst("D%d", reg);
    cpu->dst.l = M68K_LOCATION_DR;
    cpu->dst.n = reg;
    break;

  case EA_MODE_AR_DIRECT: /* An */
    m68k_trace_op_dst("A%d", reg);
    cpu->dst.l = M68K_LOCATION_AR;
    cpu->dst.n = reg;
    break;

  case EA_MODE_AR_INDIRECT: /* (An) */
    m68k_trace_op_dst("(A%d)", reg);
    address = m68k_address_reg_value(cpu, reg);
    cpu->dst.l = M68K_LOCATION_MEM;
    cpu->dst.n = address;
    break;

  case EA_MODE_AR_POST_INC: /* (An)+ */
    m68k_trace_op_dst("(A%d)+", reg);
    address = m68k_address_reg_value(cpu, reg);
    cpu->dst.l = M68K_LOCATION_MEM;
    cpu->dst.n = address;
    m68k_address_reg_inc(cpu, reg, width);
    break;

  case EA_MODE_AR_PRE_DEC: /* -(An) */
    m68k_trace_op_dst("-(A%d)", reg);
    m68k_address_reg_dec(cpu, reg, width);
    address = m68k_address_reg_value(cpu, reg);
    cpu->dst.l = M68K_LOCATION_MEM;
    cpu->dst.n = address;
    break;

  case EA_MODE_AR_DISP_16: /* (d16,An) */
    m68k_trace_op_dst("(d16, A%d)", reg);
    ext_word = m68k_fetch(cpu, mem);
    address = m68k_address_reg_value(cpu, reg);
    address += (int16_t)(ext_word & 0xFFFF);
    cpu->dst.l = M68K_LOCATION_MEM;
    cpu->dst.n = address;
    break;

  case EA_MODE_AR_DISP_8: /* (d8,An,Xn) */
    m68k_trace_op_dst("(d8, A%d, Xn)", reg);
    ext_word = m68k_fetch(cpu, mem);
    address = m68k_address_reg_value(cpu, reg);
    address += (int8_t)(ext_word & 0xFF);
    address += m68k_ext_word_reg_value(cpu, ext_word);
    cpu->dst.l = M68K_LOCATION_MEM;
    cpu->dst.n = address;
    break;

  case EA_MODE_EXT:
    switch (reg) {
    case EA_MODE_EXT_ABS_WORD: /* (xxx).W */
      address = (int16_t)m68k_fetch(cpu, mem);
      m68k_trace_op_dst("($%08x).W", address);
      cpu->dst.l = M68K_LOCATION_MEM;
      cpu->dst.n = address;
      break;

    case EA_MODE_EXT_ABS_LONG: /* (xxx).L */
      address = (m68k_fetch(cpu, mem) << 16);
      address += m68k_fetch(cpu, mem);
      m68k_trace_op_dst("($%08x).L", address);
      cpu->dst.l = M68K_LOCATION_MEM;
      cpu->dst.n = address;
      break;

    case EA_MODE_EXT_PC_DISP_16: /* (d16,PC) */
      m68k_trace_op_dst("(d16, PC)");
      ext_word = m68k_fetch(cpu, mem);
      address = cpu->pc - 2;
      address += (int16_t)(ext_word & 0xFFFF);
      cpu->dst.l = M68K_LOCATION_MEM;
      cpu->dst.n = address;
      cpu->dst.program_space = true;
      break;

    case EA_MODE_EXT_PC_DISP_8: /* (d8,PC,Xn) */
      m68k_trace_op_dst("(d8, PC, Xn)");
      ext_word = m68k_fetch(cpu, mem);
      address = cpu->pc - 2;
      address += (int8_t)(ext_word & 0xFF);
      address += m68k_ext_word_reg_value(cpu, ext_word);
      cpu->dst.l = M68K_LOCATION_MEM;
      cpu->dst.n = address;
      cpu->dst.program_space = true;
      break;

    case EA_MODE_EXT_IMMEDIATE: /* #<data> */
      cpu->dst.l = M68K_LOCATION_IMM;
      if (width == 4) {
        ext_word = m68k_fetch(cpu, mem);
        cpu->dst.n = (ext_word & 0xFFFF) << 16;
        ext_word = m68k_fetch(cpu, mem);
        cpu->dst.n |= ext_word & 0xFFFF;
        m68k_trace_op_dst("#$%08x", cpu->src.n);
      } else {
        ext_word = m68k_fetch(cpu, mem);
        cpu->dst.n = ext_word;
        if (width == 2) {
          m68k_trace_op_dst("#$%04x", cpu->dst.n);
        } else {
          m68k_trace_op_dst("#$%02x", cpu->dst.n & 0xFF);
        }
      }
      break;

    default:
      /* Unhandled effective address destination. */
      m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
      break;
    }
    break;
  }
}



static uint8_t m68k_dst_read_byte(m68k_t *cpu, mem_t *mem)
{
  uint8_t value = 0;

  switch (cpu->dst.l) {
  case M68K_LOCATION_DR:
    value = cpu->d[cpu->dst.n] & 0xFF;
    break;

  case M68K_LOCATION_MEM:
    value = mem_read_byte(mem, cpu->dst.n);
    break;

  case M68K_LOCATION_IMM:
    value = cpu->dst.n & 0xFF;
    break;

  default:
    /* Unhandled destination byte read. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }

  return value;
}



static uint16_t m68k_dst_read_word(m68k_t *cpu, mem_t *mem)
{
  uint16_t value = 0;
  bool error = false;

  switch (cpu->dst.l) {
  case M68K_LOCATION_DR:
    value = cpu->d[cpu->dst.n] & 0xFFFF;
    break;

  case M68K_LOCATION_AR:
    value = m68k_address_reg_value(cpu, cpu->dst.n) & 0xFFFF;
    break;

  case M68K_LOCATION_MEM:
    value = mem_read_word(mem, cpu->dst.n, &error);
    if (error) {
      m68k_address_error(cpu, mem, cpu->dst.n, true, cpu->dst.program_space);
    }
    break;

  default:
    /* Unhandled destination word read. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }

  return value;
}



static uint32_t m68k_dst_read_long(m68k_t *cpu, mem_t *mem)
{
  uint32_t value = 0;
  bool error = false;

  switch (cpu->dst.l) {
  case M68K_LOCATION_DR:
    value = cpu->d[cpu->dst.n];
    break;

  case M68K_LOCATION_AR:
    value = m68k_address_reg_value(cpu, cpu->dst.n);
    break;

  case M68K_LOCATION_MEM:
    value = mem_read_long(mem, cpu->dst.n, &error);
    if (error) {
      m68k_address_error(cpu, mem, cpu->dst.n, true, cpu->dst.program_space);
    }
    break;

  default:
    /* Unhandled destination long read. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }

  return value;
}



static void m68k_dst_write_byte(m68k_t *cpu, mem_t *mem, uint8_t value)
{
  switch (cpu->dst.l) {
  case M68K_LOCATION_DR:
    cpu->d[cpu->dst.n] &= ~0xFF;
    cpu->d[cpu->dst.n] |= value;
    break;

  case M68K_LOCATION_MEM:
    mem_write_byte(mem, cpu->dst.n, value);
    break;

  default:
    /* Unhandled destination byte write. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }
}



static void m68k_dst_write_word(m68k_t *cpu, mem_t *mem, uint16_t value)
{
  bool error = false;

  switch (cpu->dst.l) {
  case M68K_LOCATION_DR:
    cpu->d[cpu->dst.n] &= ~0xFFFF;
    cpu->d[cpu->dst.n] |= value;
    break;

  case M68K_LOCATION_AR:
    m68k_address_reg_set_word(cpu, cpu->dst.n, value);
    break;

  case M68K_LOCATION_MEM:
    mem_write_word(mem, cpu->dst.n, value, &error);
    if (error) {
      m68k_address_error(cpu, mem, cpu->dst.n, false, cpu->dst.program_space);
    }
    break;

  default:
    /* Unhandled destination word write. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }
}



static void m68k_dst_write_long(m68k_t *cpu, mem_t *mem, uint32_t value)
{
  bool error = false;

  switch (cpu->dst.l) {
  case M68K_LOCATION_DR:
    cpu->d[cpu->dst.n] = value;
    break;

  case M68K_LOCATION_AR:
    m68k_address_reg_set_long(cpu, cpu->dst.n, value);
    break;

  case M68K_LOCATION_MEM:
    mem_write_long(mem, cpu->dst.n, value, &error);
    if (error) {
      m68k_address_error(cpu, mem, cpu->dst.n, false, cpu->dst.program_space);
    }
    break;

  default:
    /* Unhandled destination long write. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }
}



static void m68k_addx(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  bool error = false;
  uint32_t address;
  uint32_t dst_value;
  uint32_t src_value;
  uint8_t reg_y =  opcode       & 0b111;
  uint8_t rm    = (opcode >> 3) & 0b1;
  uint8_t size  = (opcode >> 6) & 0b11;
  uint8_t reg_x = (opcode >> 9) & 0b111;

  switch (size) {
  case 0b00:
    m68k_trace_op_mnemonic("ADDX.B");
    if (rm) { /* -(Ay), -(Ax) */
      m68k_trace_op_src("-(A%d)", reg_y);
      m68k_trace_op_dst("-(A%d)", reg_x);
      m68k_address_reg_dec(cpu, reg_y, 1);
      address = m68k_address_reg_value(cpu, reg_y);
      src_value = mem_read_byte(mem, address);
      m68k_address_reg_dec(cpu, reg_x, 1);
      address = m68k_address_reg_value(cpu, reg_x);
      dst_value = mem_read_byte(mem, address);
      dst_value = m68k_addx_byte(cpu, src_value, dst_value);
      mem_write_byte(mem, address, dst_value);

    } else { /* Dy, Dx */
      m68k_trace_op_src("D%d", reg_y);
      m68k_trace_op_dst("D%d", reg_x);
      dst_value = m68k_addx_byte(cpu, cpu->d[reg_y], cpu->d[reg_x]);
      cpu->d[reg_x] &= ~0xFF;
      cpu->d[reg_x] |= dst_value;
    }
    break;

  case 0b01:
    m68k_trace_op_mnemonic("ADDX.W");
    if (rm) { /* -(Ay), -(Ax) */
      m68k_trace_op_src("-(A%d)", reg_y);
      m68k_trace_op_dst("-(A%d)", reg_x);
      m68k_address_reg_dec(cpu, reg_y, 2);
      address = m68k_address_reg_value(cpu, reg_y);
      src_value = mem_read_word(mem, address, &error);
      if (error) {
        m68k_address_error(cpu, mem, address, true, false);
      }
      m68k_address_reg_dec(cpu, reg_x, 2);
      address = m68k_address_reg_value(cpu, reg_x);
      dst_value = mem_read_word(mem, address, &error);
      if (error) {
        m68k_address_error(cpu, mem, address, true, false);
      }
      dst_value = m68k_addx_word(cpu, src_value, dst_value);
      mem_write_word(mem, address, dst_value, &error);

    } else { /* Dy, Dx */
      m68k_trace_op_src("D%d", reg_y);
      m68k_trace_op_dst("D%d", reg_x);
      dst_value = m68k_addx_word(cpu, cpu->d[reg_y], cpu->d[reg_x]);
      cpu->d[reg_x] &= ~0xFFFF;
      cpu->d[reg_x] |= dst_value;
    }
    break;

  case 0b10:
    m68k_trace_op_mnemonic("ADDX.L");
    if (rm) { /* -(Ay), -(Ax) */
      m68k_trace_op_src("-(A%d)", reg_y);
      m68k_trace_op_dst("-(A%d)", reg_x);
      m68k_address_reg_dec(cpu, reg_y, 4);
      address = m68k_address_reg_value(cpu, reg_y);
      src_value = mem_read_long(mem, address, &error);
      if (error) {
        m68k_address_error(cpu, mem, address, true, false);
      }
      m68k_address_reg_dec(cpu, reg_x, 4);
      address = m68k_address_reg_value(cpu, reg_x);
      dst_value = mem_read_long(mem, address, &error);
      if (error) {
        m68k_address_error(cpu, mem, address, true, false);
      }
      dst_value = m68k_addx_long(cpu, src_value, dst_value);
      mem_write_long(mem, address, dst_value, &error);

    } else { /* Dy, Dx */
      m68k_trace_op_src("D%d", reg_y);
      m68k_trace_op_dst("D%d", reg_x);
      dst_value = m68k_addx_long(cpu, cpu->d[reg_y], cpu->d[reg_x]);
      cpu->d[reg_x] = dst_value;
    }
    break;

  case 0b11:
    /* Unhandled ADDX size. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }
}



static void m68k_add(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t op_mode = (opcode >> 6) & 0b111;
  uint8_t reg     = (opcode >> 9) & 0b111;

  switch (op_mode) {
  case 0b000: /* Byte, <ea> + Dn -> Dn */
    m68k_trace_op_mnemonic("ADD.B");
    m68k_trace_op_dst("D%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 1);
    value = m68k_add_byte(cpu, m68k_src_read_byte(cpu, mem), cpu->d[reg]);
    cpu->d[reg] &= ~0xFF;
    cpu->d[reg] |= value;
    break;

  case 0b001: /* Word, <ea> + Dn -> Dn */
    m68k_trace_op_mnemonic("ADD.W");
    m68k_trace_op_dst("D%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 2);
    value = m68k_add_word(cpu,
      m68k_src_read_word(cpu, mem), cpu->d[reg], false);
    cpu->d[reg] &= ~0xFFFF;
    cpu->d[reg] |= value;
    break;

  case 0b010: /* Long, <ea> + Dn -> Dn */
    m68k_trace_op_mnemonic("ADD.L");
    m68k_trace_op_dst("D%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 4);
    value = m68k_add_long(cpu,
      m68k_src_read_long(cpu, mem), cpu->d[reg], false);
    cpu->d[reg] = value;
    break;

  case 0b011: /* Word, <ea>, An */
    m68k_trace_op_mnemonic("ADDA.W");
    m68k_trace_op_dst("A%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 2);
    value = m68k_address_reg_value(cpu, reg);
    value = m68k_add_long(cpu,
      (int16_t)m68k_src_read_word(cpu, mem), value, true);
    m68k_address_reg_set_long(cpu, reg, value);
    break;

  case 0b100: /* Byte, Dn + <ea> -> <ea> */
    if (ea_mode == EA_MODE_DR_DIRECT || ea_mode == EA_MODE_AR_DIRECT) {
      m68k_addx(cpu, mem, opcode);
    } else {
      m68k_trace_op_mnemonic("ADD.B");
      m68k_trace_op_src("D%d", reg);
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
      m68k_dst_write_byte(cpu, mem,
        m68k_add_byte(cpu, cpu->d[reg],
          m68k_dst_read_byte(cpu, mem)));
    }
    break;

  case 0b101: /* Word, Dn + <ea> -> <ea> */
    if (ea_mode == EA_MODE_DR_DIRECT || ea_mode == EA_MODE_AR_DIRECT) {
      m68k_addx(cpu, mem, opcode);
    } else {
      m68k_trace_op_mnemonic("ADD.W");
      m68k_trace_op_src("D%d", reg);
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
      m68k_dst_write_word(cpu, mem,
        m68k_add_word(cpu, cpu->d[reg],
          m68k_dst_read_word(cpu, mem), false));
    }
    break;

  case 0b110: /* Long, Dn + <ea> -> <ea> */
    if (ea_mode == EA_MODE_DR_DIRECT || ea_mode == EA_MODE_AR_DIRECT) {
      m68k_addx(cpu, mem, opcode);
    } else {
      m68k_trace_op_mnemonic("ADD.L");
      m68k_trace_op_src("D%d", reg);
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 4);
      m68k_dst_write_long(cpu, mem,
        m68k_add_long(cpu, cpu->d[reg],
          m68k_dst_read_long(cpu, mem), false));
    }
    break;

  case 0b111: /* Long, <ea>, An */
    m68k_trace_op_mnemonic("ADDA.L");
    m68k_trace_op_dst("A%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 4);
    value = m68k_address_reg_value(cpu, reg);
    value = m68k_add_long(cpu,
      m68k_src_read_long(cpu, mem), value, true);
    m68k_address_reg_set_long(cpu, reg, value);
    break;
  }
}



static void m68k_addi(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t size    = (opcode >> 6) & 0b11;

  switch (size) {
  case 0b00:
    m68k_trace_op_mnemonic("ADDI.B");
    value = m68k_fetch(cpu, mem) & 0xFF;
    m68k_trace_op_src("#$%02x", value);
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
    m68k_dst_write_byte(cpu, mem,
      m68k_add_byte(cpu, value,
        m68k_dst_read_byte(cpu, mem)));
    break;

  case 0b01:
    m68k_trace_op_mnemonic("ADDI.W");
    value = m68k_fetch(cpu, mem);
    m68k_trace_op_src("#$%04x", value);
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
    m68k_dst_write_word(cpu, mem,
      m68k_add_word(cpu, value,
        m68k_dst_read_word(cpu, mem), false));
    break;

  case 0b10:
    m68k_trace_op_mnemonic("ADDI.L");
    value = m68k_fetch(cpu, mem) << 16;
    value |= m68k_fetch(cpu, mem);
    m68k_trace_op_src("#$%08x", value);
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 4);
    m68k_dst_write_long(cpu, mem,
      m68k_add_long(cpu, value,
        m68k_dst_read_long(cpu, mem), false));
    break;

  case 0b11:
    /* Unhandled ADDI size. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }
}



static void m68k_addq(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t size    = (opcode >> 6) & 0b11;
  uint8_t value   = (opcode >> 9) & 0b111;

  if (value == 0) {
    value = 8;
  }

  switch (size) {
  case 0b00:
    m68k_trace_op_mnemonic("ADDQ.B");
    m68k_trace_op_src("%d", value);
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
    m68k_dst_write_byte(cpu, mem,
      m68k_add_byte(cpu, value,
        m68k_dst_read_byte(cpu, mem)));
    break;

  case 0b01:
    m68k_trace_op_mnemonic("ADDQ.W");
    m68k_trace_op_src("%d", value);
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
    m68k_dst_write_word(cpu, mem,
      m68k_add_word(cpu, value,
        m68k_dst_read_word(cpu, mem), ea_mode == EA_MODE_AR_DIRECT));
    break;

  case 0b10:
    m68k_trace_op_mnemonic("ADDQ.L");
    m68k_trace_op_src("%d", value);
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 4);
    m68k_dst_write_long(cpu, mem,
      m68k_add_long(cpu, value,
        m68k_dst_read_long(cpu, mem), ea_mode == EA_MODE_AR_DIRECT));
    break;
  }
}



static void m68k_abcd(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t address;
  uint32_t dst_value;
  uint32_t src_value;
  uint8_t reg_y =  opcode       & 0b111;
  uint8_t rm    = (opcode >> 3) & 0b1;
  uint8_t reg_x = (opcode >> 9) & 0b111;

  m68k_trace_op_mnemonic("ABCD");

  if (rm) { /* -(Ay), -(Ax) */
    m68k_trace_op_src("-(A%d)", reg_y);
    m68k_trace_op_dst("-(A%d)", reg_x);
    m68k_address_reg_dec(cpu, reg_y, 1);
    address = m68k_address_reg_value(cpu, reg_y);
    src_value = mem_read_byte(mem, address);
    m68k_address_reg_dec(cpu, reg_x, 1);
    address = m68k_address_reg_value(cpu, reg_x);
    dst_value = mem_read_byte(mem, address);
    dst_value = m68k_add_bcd(cpu, src_value, dst_value);
    mem_write_byte(mem, address, dst_value);

  } else { /* Dy, Dx */
    m68k_trace_op_src("D%d", reg_y);
    m68k_trace_op_dst("D%d", reg_x);
    dst_value = m68k_add_bcd(cpu, cpu->d[reg_y], cpu->d[reg_x]);
    cpu->d[reg_x] &= ~0xFF;
    cpu->d[reg_x] |= dst_value;
  }
}



static void m68k_exg(m68k_t *cpu, uint16_t opcode)
{
  uint32_t temp;
  uint8_t reg_y  =  opcode       & 0b111;
  uint8_t opmode = (opcode >> 3) & 0b11111;
  uint8_t reg_x  = (opcode >> 9) & 0b111;

  m68k_trace_op_mnemonic("EXG");

  switch (opmode) {
  case 0b01000:
    m68k_trace_op_src("D%d", reg_x);
    m68k_trace_op_dst("D%d", reg_y);
    temp = cpu->d[reg_y];
    cpu->d[reg_y] = cpu->d[reg_x];
    cpu->d[reg_x] = temp;
    break;

  case 0b01001:
    m68k_trace_op_src("A%d", reg_x);
    m68k_trace_op_dst("A%d", reg_y);
    temp = m68k_address_reg_value(cpu, reg_y);
    m68k_address_reg_set_long(cpu, reg_y, m68k_address_reg_value(cpu, reg_x));
    m68k_address_reg_set_long(cpu, reg_x, temp);
    break;

  case 0b10001:
    m68k_trace_op_src("D%d", reg_x);
    m68k_trace_op_dst("A%d", reg_y);
    temp = cpu->d[reg_x];
    cpu->d[reg_x] = m68k_address_reg_value(cpu, reg_y);
    m68k_address_reg_set_long(cpu, reg_y, temp);
    break;
  }
}



static void m68k_muls(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t reg     = (opcode >> 9) & 0b111;

  m68k_trace_op_mnemonic("MULS");
  m68k_trace_op_dst("D%d", reg);
  m68k_src_set(cpu, mem, ea_reg, ea_mode, 2);
  cpu->d[reg] = (int16_t)m68k_src_read_word(cpu, mem) *
                (int16_t)(cpu->d[reg] & 0xFFFF);
  cpu->status.n = cpu->d[reg] >> 31;
  cpu->status.z = cpu->d[reg] == 0;
  cpu->status.v = 0;
  cpu->status.c = 0;
}



static void m68k_mulu(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t reg     = (opcode >> 9) & 0b111;

  m68k_trace_op_mnemonic("MULU");
  m68k_trace_op_dst("D%d", reg);
  m68k_src_set(cpu, mem, ea_reg, ea_mode, 2);
  cpu->d[reg] = m68k_src_read_word(cpu, mem) * (cpu->d[reg] & 0xFFFF);
  cpu->status.n = cpu->d[reg] >> 31;
  cpu->status.z = cpu->d[reg] == 0;
  cpu->status.v = 0;
  cpu->status.c = 0;
}



static void m68k_and(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t op_mode = (opcode >> 6) & 0b111;
  uint8_t reg     = (opcode >> 9) & 0b111;

  switch (op_mode) {
  case 0b000: /* Byte, <ea> & Dn -> Dn */
    m68k_trace_op_mnemonic("AND.B");
    m68k_trace_op_dst("D%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 1);
    value = m68k_and_byte(cpu, m68k_src_read_byte(cpu, mem), cpu->d[reg]);
    cpu->d[reg] &= ~0xFF;
    cpu->d[reg] |= value;
    break;

  case 0b001: /* Word, <ea> & Dn -> Dn */
    m68k_trace_op_mnemonic("AND.W");
    m68k_trace_op_dst("D%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 2);
    value = m68k_and_word(cpu,
      m68k_src_read_word(cpu, mem), cpu->d[reg]);
    cpu->d[reg] &= ~0xFFFF;
    cpu->d[reg] |= value;
    break;

  case 0b010: /* Long, <ea> & Dn -> Dn */
    m68k_trace_op_mnemonic("AND.L");
    m68k_trace_op_dst("D%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 4);
    value = m68k_and_long(cpu,
      m68k_src_read_long(cpu, mem), cpu->d[reg]);
    cpu->d[reg] = value;
    break;

  case 0b011:
    m68k_mulu(cpu, mem, opcode);
    break;

  case 0b100: /* Byte, Dn & <ea> -> <ea> */
    if (ea_mode == EA_MODE_DR_DIRECT || ea_mode == EA_MODE_AR_DIRECT) {
      m68k_abcd(cpu, mem, opcode);
    } else {
      m68k_trace_op_mnemonic("AND.B");
      m68k_trace_op_src("D%d", reg);
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
      m68k_dst_write_byte(cpu, mem,
        m68k_and_byte(cpu, cpu->d[reg],
          m68k_dst_read_byte(cpu, mem)));
    }
    break;

  case 0b101: /* Word, Dn & <ea> -> <ea> */
    if (ea_mode == EA_MODE_DR_DIRECT || ea_mode == EA_MODE_AR_DIRECT) {
      m68k_exg(cpu, opcode);
    } else {
      m68k_trace_op_mnemonic("AND.W");
      m68k_trace_op_src("D%d", reg);
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
      m68k_dst_write_word(cpu, mem,
        m68k_and_word(cpu, cpu->d[reg],
          m68k_dst_read_word(cpu, mem)));
    }
    break;

  case 0b110: /* Long, Dn & <ea> -> <ea> */
    if (ea_mode == EA_MODE_DR_DIRECT) {
      /* Unhandled AND sub-instruction. */
      m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    } else if (ea_mode == EA_MODE_AR_DIRECT) {
      m68k_exg(cpu, opcode);
    } else {
      m68k_trace_op_mnemonic("AND.L");
      m68k_trace_op_src("D%d", reg);
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 4);
      m68k_dst_write_long(cpu, mem,
        m68k_and_long(cpu, cpu->d[reg],
          m68k_dst_read_long(cpu, mem)));
    }
    break;

  case 0b111:
    m68k_muls(cpu, mem, opcode);
    break;
  }
}



static void m68k_andi(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t size    = (opcode >> 6) & 0b11;

  switch (size) {
  case 0b00:
    m68k_trace_op_mnemonic("ANDI.B");
    value = m68k_fetch(cpu, mem) & 0xFF;
    m68k_trace_op_src("#$%02x", value);
    if ((ea_mode == EA_MODE_EXT) && (ea_reg == EA_MODE_EXT_IMMEDIATE)) {
      m68k_trace_op_dst("CCR");
      cpu->sr &= (0xFF00 + value);
    } else {
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
      m68k_dst_write_byte(cpu, mem,
        m68k_and_byte(cpu, value,
          m68k_dst_read_byte(cpu, mem)));
    }
    break;

  case 0b01:
    m68k_trace_op_mnemonic("ANDI.W");
    value = m68k_fetch(cpu, mem);
    m68k_trace_op_src("#$%04x", value);
    if ((ea_mode == EA_MODE_EXT) && (ea_reg == EA_MODE_EXT_IMMEDIATE)) {
      m68k_trace_op_dst("SR");
      if (cpu->status.s) {
        cpu->sr &= value;
      } else {
        m68k_exception(cpu, mem, M68K_VECTOR_PRIVILEGE_VIOLATION);
      }
    } else {
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
      m68k_dst_write_word(cpu, mem,
        m68k_and_word(cpu, value,
          m68k_dst_read_word(cpu, mem)));
    }
    break;

  case 0b10:
    m68k_trace_op_mnemonic("ANDI.L");
    value = m68k_fetch(cpu, mem) << 16;
    value |= m68k_fetch(cpu, mem);
    m68k_trace_op_src("#$%08x", value);
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 4);
    m68k_dst_write_long(cpu, mem,
      m68k_and_long(cpu, value,
        m68k_dst_read_long(cpu, mem)));
    break;

  case 0b11:
    /* Unhandled ANDI size. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }
}



static void m68k_as_reg_byte(m68k_t *cpu, uint16_t opcode)
{
  uint8_t value;
  uint8_t reg   =  opcode       & 0b111;
  uint8_t ir    = (opcode >> 5) & 0b1;
  uint8_t dr    = (opcode >> 8) & 0b1;
  uint8_t count = (opcode >> 9) & 0b111;

  m68k_trace_op_dst("D%d", reg);
  if (ir) {
    m68k_trace_op_src("D%d", count);
    count = cpu->d[count] % 64;
  } else {
    m68k_trace_op_src("#%d", count);
    if (count == 0) {
      count = 8;
    }
  }
  if (dr) {
    m68k_trace_op_mnemonic("ASL.B");
    value = m68k_asl_byte(cpu, cpu->d[reg] & 0xFF, count);
  } else {
    m68k_trace_op_mnemonic("ASR.B");
    value = m68k_asr_byte(cpu, cpu->d[reg] & 0xFF, count);
  }
  cpu->d[reg] &= ~0xFF;
  cpu->d[reg] |= value;
}



static void m68k_as_reg_word(m68k_t *cpu, uint16_t opcode)
{
  uint16_t value;
  uint8_t reg   =  opcode       & 0b111;
  uint8_t ir    = (opcode >> 5) & 0b1;
  uint8_t dr    = (opcode >> 8) & 0b1;
  uint8_t count = (opcode >> 9) & 0b111;

  m68k_trace_op_dst("D%d", reg);
  if (ir) {
    m68k_trace_op_src("D%d", count);
    count = cpu->d[count] % 64;
  } else {
    m68k_trace_op_src("#%d", count);
    if (count == 0) {
      count = 8;
    }
  }
  if (dr) {
    m68k_trace_op_mnemonic("ASL.W");
    value = m68k_asl_word(cpu, cpu->d[reg] & 0xFFFF, count);
  } else {
    m68k_trace_op_mnemonic("ASR.W");
    value = m68k_asr_word(cpu, cpu->d[reg] & 0xFFFF, count);
  }
  cpu->d[reg] &= ~0xFFFF;
  cpu->d[reg] |= value;
}



static void m68k_as_reg_long(m68k_t *cpu, uint16_t opcode)
{
  uint8_t reg   =  opcode       & 0b111;
  uint8_t ir    = (opcode >> 5) & 0b1;
  uint8_t dr    = (opcode >> 8) & 0b1;
  uint8_t count = (opcode >> 9) & 0b111;

  m68k_trace_op_dst("D%d", reg);
  if (ir) {
    m68k_trace_op_src("D%d", count);
    count = cpu->d[count] % 64;
  } else {
    m68k_trace_op_src("#%d", count);
    if (count == 0) {
      count = 8;
    }
  }
  if (dr) {
    m68k_trace_op_mnemonic("ASL.L");
    cpu->d[reg] = m68k_asl_long(cpu, cpu->d[reg], count);
  } else {
    m68k_trace_op_mnemonic("ASR.L");
    cpu->d[reg] = m68k_asr_long(cpu, cpu->d[reg], count);
  }
}



static void m68k_as_mem(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t dr      = (opcode >> 8) & 0b1;

  m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
  if (dr) {
    m68k_trace_op_mnemonic("ASL.W");
    m68k_dst_write_word(cpu, mem,
      m68k_asl_word(cpu,
        m68k_dst_read_word(cpu, mem), 1));
  } else {
    m68k_trace_op_mnemonic("ASR.W");
    m68k_dst_write_word(cpu, mem,
      m68k_asr_word(cpu,
        m68k_dst_read_word(cpu, mem), 1));
  }
}



static void m68k_branch(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  bool branch = false;
  bool word = false;
  uint32_t address;
  int16_t disp = (int8_t)(opcode & 0xFF);
  uint8_t cond = (opcode >> 8) & 0b1111;

  if (disp == 0) {
    disp = (int16_t)m68k_fetch(cpu, mem);
    address = cpu->pc + (disp - 2);
    word = true;
  } else {
    address = cpu->pc + disp;
  }

  m68k_trace_op_dst("$%08x", address);

  switch (cond) {
  case 0b0000:
    m68k_trace_op_mnemonic("BRA");
    branch = true;
    break;

  case 0b0001:
    m68k_trace_op_mnemonic("BSR");
    branch = true;
    m68k_stack_push(cpu, mem, cpu->pc % 0x10000);
    m68k_stack_push(cpu, mem, cpu->pc / 0x10000);
    break;

  case 0b0010:
    m68k_trace_op_mnemonic("BHI");
    if (cpu->status.c == 0 && cpu->status.z == 0) {
      branch = true;
    }
    break;

  case 0b0011:
    m68k_trace_op_mnemonic("BLS");
    if (cpu->status.c == 1 || cpu->status.z == 1) {
      branch = true;
    }
    break;

  case 0b0100:
    m68k_trace_op_mnemonic("BCC");
    if (cpu->status.c == 0) {
      branch = true;
    }
    break;

  case 0b0101:
    m68k_trace_op_mnemonic("BCS");
    if (cpu->status.c == 1) {
      branch = true;
    }
    break;

  case 0b0110:
    m68k_trace_op_mnemonic("BNE");
    if (cpu->status.z == 0) {
      branch = true;
    }
    break;

  case 0b0111:
    m68k_trace_op_mnemonic("BEQ");
    if (cpu->status.z == 1) {
      branch = true;
    }
    break;

  case 0b1000:
    m68k_trace_op_mnemonic("BVC");
    if (cpu->status.v == 0) {
      branch = true;
    }
    break;

  case 0b1001:
    m68k_trace_op_mnemonic("BVS");
    if (cpu->status.v == 1) {
      branch = true;
    }
    break;

  case 0b1010:
    m68k_trace_op_mnemonic("BPL");
    if (cpu->status.n == 0) {
      branch = true;
    }
    break;

  case 0b1011:
    m68k_trace_op_mnemonic("BMI");
    if (cpu->status.n == 1) {
      branch = true;
    }
    break;

  case 0b1100:
    m68k_trace_op_mnemonic("BGE");
    if ((cpu->status.n == 1 && cpu->status.v == 1) ||
        (cpu->status.n == 0 && cpu->status.v == 0))
    {
      branch = true;
    }
    break;

  case 0b1101:
    m68k_trace_op_mnemonic("BLT");
    if ((cpu->status.n == 0 && cpu->status.v == 1) ||
        (cpu->status.n == 1 && cpu->status.v == 0))
    {
      branch = true;
    }
    break;

  case 0b1110:
    m68k_trace_op_mnemonic("BGT");
    if ((cpu->status.n == 0 && cpu->status.v == 0 && cpu->status.z == 0) ||
        (cpu->status.n == 1 && cpu->status.v == 1 && cpu->status.z == 0))
    {
      branch = true;
    }
    break;

  case 0b1111:
    m68k_trace_op_mnemonic("BLE");
    if ((cpu->status.z == 1) ||
        (cpu->status.n == 1 && cpu->status.v == 0) ||
        (cpu->status.n == 0 && cpu->status.v == 1))
    {
      branch = true;
    }
    break;
  }

  if (branch) {
    if (address % 2 != 0) {
      if (cond == 0b0001) { /* BSR */
        cpu->pc = address;
        m68k_address_error(cpu, mem, cpu->pc, true, true);
      } else {
        if (word) {
          cpu->pc -= 2;
        }
        m68k_address_error(cpu, mem, address, true, true);
      }
    } else {
      cpu->pc = address;
    }
  }
}



static void m68k_bchg_imm(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint16_t bit_no;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;

  m68k_trace_op_mnemonic("BCHG");
  bit_no = m68k_fetch(cpu, mem);
  m68k_trace_op_src("#%d", bit_no);
  if (ea_mode == EA_MODE_DR_DIRECT) {
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 8);
    bit_no %= 32;
    value = m68k_dst_read_long(cpu, mem);
    if ((value >> bit_no) & 1) {
      cpu->status.z = 0;
      value &= ~(1 << bit_no);
    } else {
      cpu->status.z = 1;
      value |= (1 << bit_no);
    }
    m68k_dst_write_long(cpu, mem, value);
  } else {
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
    bit_no %= 8;
    value = m68k_dst_read_byte(cpu, mem);
    if ((value >> bit_no) & 1) {
      cpu->status.z = 0;
      value &= ~(1 << bit_no);
    } else {
      cpu->status.z = 1;
      value |= (1 << bit_no);
    }
    m68k_dst_write_byte(cpu, mem, value);
  }
}



static void m68k_bchg_reg(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint32_t bit_no;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t reg     = (opcode >> 9) & 0b111;

  m68k_trace_op_mnemonic("BCHG");
  m68k_trace_op_src("D%d", reg);
  bit_no = cpu->d[reg];
  if (ea_mode == EA_MODE_DR_DIRECT) {
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 8);
    bit_no %= 32;
    value = m68k_dst_read_long(cpu, mem);
    if ((value >> bit_no) & 1) {
      cpu->status.z = 0;
      value &= ~(1 << bit_no);
    } else {
      cpu->status.z = 1;
      value |= (1 << bit_no);
    }
    m68k_dst_write_long(cpu, mem, value);
  } else {
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
    bit_no %= 8;
    value = m68k_dst_read_byte(cpu, mem);
    if ((value >> bit_no) & 1) {
      cpu->status.z = 0;
      value &= ~(1 << bit_no);
    } else {
      cpu->status.z = 1;
      value |= (1 << bit_no);
    }
    m68k_dst_write_byte(cpu, mem, value);
  }
}



static void m68k_bclr_imm(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint16_t bit_no;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;

  m68k_trace_op_mnemonic("BCLR");
  bit_no = m68k_fetch(cpu, mem);
  m68k_trace_op_src("#%d", bit_no);
  if (ea_mode == EA_MODE_DR_DIRECT) {
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 8);
    bit_no %= 32;
    value = m68k_dst_read_long(cpu, mem);
    if ((value >> bit_no) & 1) {
      cpu->status.z = 0;
    } else {
      cpu->status.z = 1;
    }
    value &= ~(1 << bit_no);
    m68k_dst_write_long(cpu, mem, value);
  } else {
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
    bit_no %= 8;
    value = m68k_dst_read_byte(cpu, mem);
    if ((value >> bit_no) & 1) {
      cpu->status.z = 0;
    } else {
      cpu->status.z = 1;
    }
    value &= ~(1 << bit_no);
    m68k_dst_write_byte(cpu, mem, value);
  }
}



static void m68k_bclr_reg(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint32_t bit_no;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t reg     = (opcode >> 9) & 0b111;

  m68k_trace_op_mnemonic("BCLR");
  m68k_trace_op_src("D%d", reg);
  bit_no = cpu->d[reg];
  if (ea_mode == EA_MODE_DR_DIRECT) {
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 8);
    bit_no %= 32;
    value = m68k_dst_read_long(cpu, mem);
    if ((value >> bit_no) & 1) {
      cpu->status.z = 0;
    } else {
      cpu->status.z = 1;
    }
    value &= ~(1 << bit_no);
    m68k_dst_write_long(cpu, mem, value);
  } else {
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
    bit_no %= 8;
    value = m68k_dst_read_byte(cpu, mem);
    if ((value >> bit_no) & 1) {
      cpu->status.z = 0;
    } else {
      cpu->status.z = 1;
    }
    value &= ~(1 << bit_no);
    m68k_dst_write_byte(cpu, mem, value);
  }
}



static void m68k_bset_imm(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint16_t bit_no;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;

  m68k_trace_op_mnemonic("BSET");
  bit_no = m68k_fetch(cpu, mem);
  m68k_trace_op_src("#%d", bit_no);
  if (ea_mode == EA_MODE_DR_DIRECT) {
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 8);
    bit_no %= 32;
    value = m68k_dst_read_long(cpu, mem);
    if ((value >> bit_no) & 1) {
      cpu->status.z = 0;
    } else {
      cpu->status.z = 1;
    }
    value |= (1 << bit_no);
    m68k_dst_write_long(cpu, mem, value);
  } else {
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
    bit_no %= 8;
    value = m68k_dst_read_byte(cpu, mem);
    if ((value >> bit_no) & 1) {
      cpu->status.z = 0;
    } else {
      cpu->status.z = 1;
    }
    value |= (1 << bit_no);
    m68k_dst_write_byte(cpu, mem, value);
  }
}



static void m68k_bset_reg(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint32_t bit_no;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t reg     = (opcode >> 9) & 0b111;

  m68k_trace_op_mnemonic("BSET");
  m68k_trace_op_src("D%d", reg);
  bit_no = cpu->d[reg];
  if (ea_mode == EA_MODE_DR_DIRECT) {
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 8);
    bit_no %= 32;
    value = m68k_dst_read_long(cpu, mem);
    if ((value >> bit_no) & 1) {
      cpu->status.z = 0;
    } else {
      cpu->status.z = 1;
    }
    value |= (1 << bit_no);
    m68k_dst_write_long(cpu, mem, value);
  } else {
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
    bit_no %= 8;
    value = m68k_dst_read_byte(cpu, mem);
    if ((value >> bit_no) & 1) {
      cpu->status.z = 0;
    } else {
      cpu->status.z = 1;
    }
    value |= (1 << bit_no);
    m68k_dst_write_byte(cpu, mem, value);
  }
}



static void m68k_btst_imm(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint16_t bit_no;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;

  m68k_trace_op_mnemonic("BTST");
  bit_no = m68k_fetch(cpu, mem);
  m68k_trace_op_src("#%d", bit_no);
  if (ea_mode == EA_MODE_DR_DIRECT) {
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 8);
    bit_no %= 32;
    if ((m68k_dst_read_long(cpu, mem) >> bit_no) & 1) {
      cpu->status.z = 0;
    } else {
      cpu->status.z = 1;
    }
  } else {
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
    bit_no %= 8;
    if ((m68k_dst_read_byte(cpu, mem) >> bit_no) & 1) {
      cpu->status.z = 0;
    } else {
      cpu->status.z = 1;
    }
  }
}



static void m68k_btst_reg(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t bit_no;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t reg     = (opcode >> 9) & 0b111;

  m68k_trace_op_mnemonic("BTST");
  m68k_trace_op_src("D%d", reg);
  bit_no = cpu->d[reg];
  if (ea_mode == EA_MODE_DR_DIRECT) {
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 8);
    bit_no %= 32;
    if ((m68k_dst_read_long(cpu, mem) >> bit_no) & 1) {
      cpu->status.z = 0;
    } else {
      cpu->status.z = 1;
    }
  } else {
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
    bit_no %= 8;
    if ((m68k_dst_read_byte(cpu, mem) >> bit_no) & 1) {
      cpu->status.z = 0;
    } else {
      cpu->status.z = 1;
    }
  }
}



static void m68k_chk(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  bool error = false;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t reg     = (opcode >> 9) & 0b111;

  m68k_trace_op_mnemonic("CHK");
  m68k_trace_op_dst("D%d", reg);
  m68k_src_set(cpu, mem, ea_reg, ea_mode, 2);
  cpu->status.v = 0;
  cpu->status.c = 0;
  cpu->status.z = 0;
  if ((int16_t)cpu->d[reg] < 0) {
    cpu->status.n = 1;
  } else {
    cpu->status.n = 0;
  }

  if ((int16_t)cpu->d[reg] > (int16_t)m68k_src_read_word(cpu, mem) ||
      (int16_t)cpu->d[reg] < 0) {
    m68k_ssp_push(cpu, mem, cpu->pc % 0x10000);
    m68k_ssp_push(cpu, mem, cpu->pc / 0x10000);
    m68k_ssp_push(cpu, mem, cpu->sr);
    cpu->pc = mem_read_long(mem, M68K_VECTOR_CHK_INSTRUCTION, &error);
    cpu->sr &= ~0x8000; /* Clear Trace Bit */
    cpu->sr |= 0x2000; /* Set Supervisor Bit */

    longjmp(m68k_exception_jmp, 1);
  }
}



static void m68k_clr(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t size    = (opcode >> 6) & 0b11;

  switch (size) {
  case 0b00:
    m68k_trace_op_mnemonic("CLR.B");
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
    m68k_dst_write_byte(cpu, mem, 0);
    break;

  case 0b01:
    m68k_trace_op_mnemonic("CLR.W");
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
    (void)m68k_dst_read_word(cpu, mem); /* Read access for exception. */
    m68k_dst_write_word(cpu, mem, 0);
    break;

  case 0b10:
    m68k_trace_op_mnemonic("CLR.L");
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 4);
    (void)m68k_dst_read_long(cpu, mem); /* Read access for exception. */
    m68k_dst_write_long(cpu, mem, 0);
    break;
  }

  cpu->status.n = 0;
  cpu->status.z = 1;
  cpu->status.v = 0;
  cpu->status.c = 0;
}



static void m68k_cmpm(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  bool error = false;
  uint32_t address;
  uint32_t dst_value;
  uint32_t src_value;
  uint8_t reg_y =  opcode       & 0b111;
  uint8_t size  = (opcode >> 6) & 0b11;
  uint8_t reg_x = (opcode >> 9) & 0b111;

  switch (size) {
  case 0b00:
    m68k_trace_op_mnemonic("CMPM.B");
    m68k_trace_op_src("(A%d)+", reg_y);
    m68k_trace_op_dst("(A%d)+", reg_x);
    address = m68k_address_reg_value(cpu, reg_y);
    m68k_address_reg_inc(cpu, reg_y, 1);
    src_value = mem_read_byte(mem, address);
    address = m68k_address_reg_value(cpu, reg_x);
    m68k_address_reg_inc(cpu, reg_x, 1);
    dst_value = mem_read_byte(mem, address);
    m68k_cmp_byte(cpu, src_value, dst_value);
    break;

  case 0b01:
    m68k_trace_op_mnemonic("CMPM.W");
    m68k_trace_op_src("(A%d)+", reg_y);
    m68k_trace_op_dst("(A%d)+", reg_x);
    address = m68k_address_reg_value(cpu, reg_y);
    m68k_address_reg_inc(cpu, reg_y, 2);
    src_value = mem_read_word(mem, address, &error);
    if (error) {
      m68k_address_error(cpu, mem, address, true, false);
    }
    address = m68k_address_reg_value(cpu, reg_x);
    m68k_address_reg_inc(cpu, reg_x, 2);
    dst_value = mem_read_word(mem, address, &error);
    if (error) {
      m68k_address_error(cpu, mem, address, true, false);
    }
    m68k_cmp_word(cpu, src_value, dst_value);
    break;

  case 0b10:
    m68k_trace_op_mnemonic("CMPM.L");
    m68k_trace_op_src("(A%d)+", reg_y);
    m68k_trace_op_dst("(A%d)+", reg_x);
    address = m68k_address_reg_value(cpu, reg_y);
    m68k_address_reg_inc(cpu, reg_y, 4);
    src_value = mem_read_long(mem, address, &error);
    if (error) {
      m68k_address_error(cpu, mem, address, true, false);
    }
    address = m68k_address_reg_value(cpu, reg_x);
    m68k_address_reg_inc(cpu, reg_x, 4);
    dst_value = mem_read_long(mem, address, &error);
    if (error) {
      m68k_address_error(cpu, mem, address, true, false);
    }
    m68k_cmp_long(cpu, src_value, dst_value);
    break;

  case 0b11:
    /* Unhandled CMPM size. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }
}



static void m68k_cmp_eor(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t op_mode = (opcode >> 6) & 0b111;
  uint8_t reg     = (opcode >> 9) & 0b111;

  switch (op_mode) {
  case 0b000:
    m68k_trace_op_mnemonic("CMP.B");
    m68k_trace_op_dst("D%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 1);
    m68k_cmp_byte(cpu, m68k_src_read_byte(cpu, mem), cpu->d[reg]);
    break;

  case 0b001:
    m68k_trace_op_mnemonic("CMP.W");
    m68k_trace_op_dst("D%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 2);
    m68k_cmp_word(cpu, m68k_src_read_word(cpu, mem), cpu->d[reg]);
    break;

  case 0b010:
    m68k_trace_op_mnemonic("CMP.L");
    m68k_trace_op_dst("D%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 4);
    m68k_cmp_long(cpu, m68k_src_read_long(cpu, mem), cpu->d[reg]);
    break;

  case 0b011:
    m68k_trace_op_mnemonic("CMPA.W");
    m68k_trace_op_dst("A%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 2);
    value = m68k_address_reg_value(cpu, reg);
    m68k_cmp_long(cpu, (int16_t)m68k_src_read_word(cpu, mem), value);
    break;

  case 0b100:
    if (ea_mode == EA_MODE_AR_DIRECT) {
      m68k_cmpm(cpu, mem, opcode);
    } else {
      m68k_trace_op_mnemonic("EOR.B");
      m68k_trace_op_src("D%d", reg);
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
      m68k_dst_write_byte(cpu, mem,
        m68k_eor_byte(cpu, cpu->d[reg],
          m68k_dst_read_byte(cpu, mem)));
    }
    break;

  case 0b101:
    if (ea_mode == EA_MODE_AR_DIRECT) {
      m68k_cmpm(cpu, mem, opcode);
    } else {
      m68k_trace_op_mnemonic("EOR.W");
      m68k_trace_op_src("D%d", reg);
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
      m68k_dst_write_word(cpu, mem,
        m68k_eor_word(cpu, cpu->d[reg],
          m68k_dst_read_word(cpu, mem)));
    }
    break;

  case 0b110:
    if (ea_mode == EA_MODE_AR_DIRECT) {
      m68k_cmpm(cpu, mem, opcode);
    } else {
      m68k_trace_op_mnemonic("EOR.L");
      m68k_trace_op_src("D%d", reg);
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 4);
      m68k_dst_write_long(cpu, mem,
        m68k_eor_long(cpu, cpu->d[reg],
          m68k_dst_read_long(cpu, mem)));
    }
    break;

  case 0b111:
    m68k_trace_op_mnemonic("CMPA.L");
    m68k_trace_op_dst("A%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 4);
    value = m68k_address_reg_value(cpu, reg);
    m68k_cmp_long(cpu, m68k_src_read_long(cpu, mem), value);
    break;
  }
}



static void m68k_cmpi(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t size    = (opcode >> 6) & 0b11;

  switch (size) {
  case 0b00:
    m68k_trace_op_mnemonic("CMPI.B");
    value = m68k_fetch(cpu, mem) & 0xFF;
    m68k_trace_op_src("#$%02x", value);
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
    m68k_cmp_byte(cpu, value, m68k_dst_read_byte(cpu, mem));
    break;

  case 0b01:
    m68k_trace_op_mnemonic("CMPI.W");
    value = m68k_fetch(cpu, mem);
    m68k_trace_op_src("#$%04x", value);
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
    m68k_cmp_word(cpu, value, m68k_dst_read_word(cpu, mem));
    break;

  case 0b10:
    m68k_trace_op_mnemonic("CMPI.L");
    value = m68k_fetch(cpu, mem) << 16;
    value |= m68k_fetch(cpu, mem);
    m68k_trace_op_src("#$%08x", value);
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 4);
    m68k_cmp_long(cpu, value, m68k_dst_read_long(cpu, mem));
    break;

  case 0b11:
    /* Unhandled CMPI size. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }
}



static void m68k_dbcc(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  bool result = false;
  uint32_t address;
  uint16_t value;
  int16_t disp;
  uint8_t reg  =  opcode       & 0b111;
  uint8_t cond = (opcode >> 8) & 0b1111;

  disp = (int16_t)m68k_fetch(cpu, mem);
  address = cpu->pc + (disp - 2);

  m68k_trace_op_src("D%d", reg);
  m68k_trace_op_dst("$%08x", address);

  switch (cond) {
  case 0b0000:
    m68k_trace_op_mnemonic("DT");
    result = true;
    break;

  case 0b0001:
    m68k_trace_op_mnemonic("DF");
    result = false;
    break;

  case 0b0010:
    m68k_trace_op_mnemonic("DHI");
    if (cpu->status.c == 0 && cpu->status.z == 0) {
      result = true;
    }
    break;

  case 0b0011:
    m68k_trace_op_mnemonic("DLS");
    if (cpu->status.c == 1 || cpu->status.z == 1) {
      result = true;
    }
    break;

  case 0b0100:
    m68k_trace_op_mnemonic("DCC");
    if (cpu->status.c == 0) {
      result = true;
    }
    break;

  case 0b0101:
    m68k_trace_op_mnemonic("DCS");
    if (cpu->status.c == 1) {
      result = true;
    }
    break;

  case 0b0110:
    m68k_trace_op_mnemonic("DNE");
    if (cpu->status.z == 0) {
      result = true;
    }
    break;

  case 0b0111:
    m68k_trace_op_mnemonic("DEQ");
    if (cpu->status.z == 1) {
      result = true;
    }
    break;

  case 0b1000:
    m68k_trace_op_mnemonic("DVC");
    if (cpu->status.v == 0) {
      result = true;
    }
    break;

  case 0b1001:
    m68k_trace_op_mnemonic("DVS");
    if (cpu->status.v == 1) {
      result = true;
    }
    break;

  case 0b1010:
    m68k_trace_op_mnemonic("DPL");
    if (cpu->status.n == 0) {
      result = true;
    }
    break;

  case 0b1011:
    m68k_trace_op_mnemonic("DMI");
    if (cpu->status.n == 1) {
      result = true;
    }
    break;

  case 0b1100:
    m68k_trace_op_mnemonic("DGE");
    if ((cpu->status.n == 1 && cpu->status.v == 1) ||
        (cpu->status.n == 0 && cpu->status.v == 0))
    {
      result = true;
    }
    break;

  case 0b1101:
    m68k_trace_op_mnemonic("DLT");
    if ((cpu->status.n == 0 && cpu->status.v == 1) ||
        (cpu->status.n == 1 && cpu->status.v == 0))
    {
      result = true;
    }
    break;

  case 0b1110:
    m68k_trace_op_mnemonic("DGT");
    if ((cpu->status.n == 0 && cpu->status.v == 0 && cpu->status.z == 0) ||
        (cpu->status.n == 1 && cpu->status.v == 1 && cpu->status.z == 0))
    {
      result = true;
    }
    break;

  case 0b1111:
    m68k_trace_op_mnemonic("DLE");
    if ((cpu->status.z == 1) ||
        (cpu->status.n == 1 && cpu->status.v == 0) ||
        (cpu->status.n == 0 && cpu->status.v == 1))
    {
      result = true;
    }
    break;
  }

  if (result == false) {
    value = cpu->d[reg] & 0xFFFF;
    value--;
    cpu->d[reg] &= ~0xFFFF;
    cpu->d[reg] |= value;
    if ((cpu->d[reg] & 0xFFFF) != 0xFFFF) {
      if (address % 2 != 0) {
        value++;
        cpu->d[reg] &= ~0xFFFF;
        cpu->d[reg] |= value;
        cpu->pc -= 2;
        m68k_address_error(cpu, mem, address, true, true);
      } else {
        cpu->pc = address;
      }
    }
  }
}



static void m68k_eori(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t size    = (opcode >> 6) & 0b11;

  switch (size) {
  case 0b00:
    m68k_trace_op_mnemonic("EORI.B");
    value = m68k_fetch(cpu, mem) & 0xFF;
    m68k_trace_op_src("#$%02x", value);
    if ((ea_mode == EA_MODE_EXT) && (ea_reg == EA_MODE_EXT_IMMEDIATE)) {
      m68k_trace_op_dst("CCR");
      cpu->sr ^= (value & 0x1F);
    } else {
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
      m68k_dst_write_byte(cpu, mem,
        m68k_eor_byte(cpu, value,
          m68k_dst_read_byte(cpu, mem)));
    }
    break;

  case 0b01:
    m68k_trace_op_mnemonic("EORI.W");
    value = m68k_fetch(cpu, mem);
    m68k_trace_op_src("#$%04x", value);
    if ((ea_mode == EA_MODE_EXT) && (ea_reg == EA_MODE_EXT_IMMEDIATE)) {
      m68k_trace_op_dst("SR");
      if (cpu->status.s) {
        cpu->sr ^= m68k_sr_filter_bits(value);
      } else {
        m68k_exception(cpu, mem, M68K_VECTOR_PRIVILEGE_VIOLATION);
      }
    } else {
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
      m68k_dst_write_word(cpu, mem,
        m68k_eor_word(cpu, value,
          m68k_dst_read_word(cpu, mem)));
    }
    break;

  case 0b10:
    m68k_trace_op_mnemonic("EORI.L");
    value = m68k_fetch(cpu, mem) << 16;
    value |= m68k_fetch(cpu, mem);
    m68k_trace_op_src("#$%08x", value);
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 4);
    m68k_dst_write_long(cpu, mem,
      m68k_eor_long(cpu, value,
        m68k_dst_read_long(cpu, mem)));
    break;

  case 0b11:
    /* Unhandled EORI size. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }
}



static void m68k_ext(m68k_t *cpu, uint16_t opcode)
{
  uint16_t word_value;
  uint8_t reg    =  opcode       & 0b111;
  uint8_t opmode = (opcode >> 6) & 0b111;

  m68k_trace_op_dst("D%d", reg);

  switch (opmode) {
  case 0b010:
    m68k_trace_op_mnemonic("EXT.W");
    word_value = (int8_t)cpu->d[reg];
    cpu->d[reg] &= ~0xFFFF;
    cpu->d[reg] |= word_value;
    cpu->status.n = word_value >> 15;
    cpu->status.z = word_value == 0;
    break;

  case 0b011:
    m68k_trace_op_mnemonic("EXT.L");
    cpu->d[reg] = (int16_t)cpu->d[reg];
    cpu->status.n = cpu->d[reg] >> 31;
    cpu->status.z = cpu->d[reg] == 0;
    break;
  }

  cpu->status.v = 0;
  cpu->status.c = 0;
}



static void m68k_jmp(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;

  m68k_trace_op_mnemonic("JMP");
  m68k_src_set(cpu, mem, ea_reg, ea_mode, 4);
  if (cpu->src.n % 2 != 0) {
    m68k_address_error(cpu, mem, cpu->src.n, true, true);
  }
  cpu->pc = cpu->src.n;
}



static void m68k_jsr(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;

  m68k_trace_op_mnemonic("JSR");
  m68k_src_set(cpu, mem, ea_reg, ea_mode, 4);
  if (cpu->src.n % 2 != 0) {
    m68k_address_error(cpu, mem, cpu->src.n, true, true);
  }
  m68k_stack_push(cpu, mem, cpu->pc % 0x10000);
  m68k_stack_push(cpu, mem, cpu->pc / 0x10000);
  cpu->pc = cpu->src.n;
}



static void m68k_lea(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t reg     = (opcode >> 9) & 0b111;

  m68k_trace_op_mnemonic("LEA");
  m68k_trace_op_dst("A%d", reg);
  m68k_src_set(cpu, mem, ea_reg, ea_mode, 4);
  m68k_address_reg_set_long(cpu, reg, cpu->src.n);
}



static void m68k_link(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  int16_t disp;
  uint8_t reg = opcode & 0b111;

  m68k_trace_op_mnemonic("LINK");
  m68k_trace_op_src("A%d", reg);
  value = m68k_address_reg_value(cpu, reg);
  m68k_stack_push(cpu, mem, value % 0x10000);
  m68k_stack_push(cpu, mem, value / 0x10000);
  m68k_address_reg_set_long(cpu, reg, m68k_address_reg_value(cpu, M68K_SP));
  disp = (int16_t)m68k_fetch(cpu, mem);
  m68k_trace_op_dst("#%+hd", disp);
  m68k_address_reg_set_long(cpu, M68K_SP,
    m68k_address_reg_value(cpu, M68K_SP) + disp);
}



static void m68k_ls_reg_byte(m68k_t *cpu, uint16_t opcode)
{
  uint8_t value;
  uint8_t reg   =  opcode       & 0b111;
  uint8_t ir    = (opcode >> 5) & 0b1;
  uint8_t dr    = (opcode >> 8) & 0b1;
  uint8_t count = (opcode >> 9) & 0b111;

  m68k_trace_op_dst("D%d", reg);
  if (ir) {
    m68k_trace_op_src("D%d", count);
    count = cpu->d[count] % 64;
  } else {
    m68k_trace_op_src("#%d", count);
    if (count == 0) {
      count = 8;
    }
  }
  if (dr) {
    m68k_trace_op_mnemonic("LSL.B");
    value = m68k_lsl_byte(cpu, cpu->d[reg] & 0xFF, count);
  } else {
    m68k_trace_op_mnemonic("LSR.B");
    value = m68k_lsr_byte(cpu, cpu->d[reg] & 0xFF, count);
  }
  cpu->d[reg] &= ~0xFF;
  cpu->d[reg] |= value;
}



static void m68k_ls_reg_word(m68k_t *cpu, uint16_t opcode)
{
  uint16_t value;
  uint8_t reg   =  opcode       & 0b111;
  uint8_t ir    = (opcode >> 5) & 0b1;
  uint8_t dr    = (opcode >> 8) & 0b1;
  uint8_t count = (opcode >> 9) & 0b111;

  m68k_trace_op_dst("D%d", reg);
  if (ir) {
    m68k_trace_op_src("D%d", count);
    count = cpu->d[count] % 64;
  } else {
    m68k_trace_op_src("#%d", count);
    if (count == 0) {
      count = 8;
    }
  }
  if (dr) {
    m68k_trace_op_mnemonic("LSL.W");
    value = m68k_lsl_word(cpu, cpu->d[reg] & 0xFFFF, count);
  } else {
    m68k_trace_op_mnemonic("LSR.W");
    value = m68k_lsr_word(cpu, cpu->d[reg] & 0xFFFF, count);
  }
  cpu->d[reg] &= ~0xFFFF;
  cpu->d[reg] |= value;
}



static void m68k_ls_reg_long(m68k_t *cpu, uint16_t opcode)
{
  uint8_t reg   =  opcode       & 0b111;
  uint8_t ir    = (opcode >> 5) & 0b1;
  uint8_t dr    = (opcode >> 8) & 0b1;
  uint8_t count = (opcode >> 9) & 0b111;

  m68k_trace_op_dst("D%d", reg);
  if (ir) {
    m68k_trace_op_src("D%d", count);
    count = cpu->d[count] % 64;
  } else {
    m68k_trace_op_src("#%d", count);
    if (count == 0) {
      count = 8;
    }
  }
  if (dr) {
    m68k_trace_op_mnemonic("LSL.L");
    cpu->d[reg] = m68k_lsl_long(cpu, cpu->d[reg], count);
  } else {
    m68k_trace_op_mnemonic("LSR.L");
    cpu->d[reg] = m68k_lsr_long(cpu, cpu->d[reg], count);
  }
}



static void m68k_ls_mem(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t dr      = (opcode >> 8) & 0b1;

  m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
  if (dr) {
    m68k_trace_op_mnemonic("LSL.W");
    m68k_dst_write_word(cpu, mem,
      m68k_lsl_word(cpu,
        m68k_dst_read_word(cpu, mem), 1));
  } else {
    m68k_trace_op_mnemonic("LSR.W");
    m68k_dst_write_word(cpu, mem,
      m68k_lsr_word(cpu,
        m68k_dst_read_word(cpu, mem), 1));
  }
}



static void m68k_move_to_ccr(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint16_t value;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;

  m68k_trace_op_mnemonic("MOVE.W");
  m68k_trace_op_dst("CCR");
  m68k_src_set(cpu, mem, ea_reg, ea_mode, 2);
  value = m68k_src_read_word(cpu, mem);
  cpu->sr &= ~0x1F;
  cpu->sr |= (value & 0x1F);
}



static void m68k_move_to_sr(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint16_t value;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;

  m68k_trace_op_mnemonic("MOVE.W");
  m68k_trace_op_dst("SR");
  if (cpu->status.s) {
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 2);
    value = m68k_src_read_word(cpu, mem);
    cpu->sr = m68k_sr_filter_bits(value);
  } else {
    m68k_exception(cpu, mem, M68K_VECTOR_PRIVILEGE_VIOLATION);
  }
}



static void m68k_move_from_sr(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;

  m68k_trace_op_mnemonic("MOVE.W");
  m68k_trace_op_src("SR");
  m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
  (void)m68k_dst_read_word(cpu, mem); /* Read access for exception. */
  m68k_dst_write_word(cpu, mem, cpu->sr);
}



static void m68k_move_to_usp(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t reg = opcode & 0b111;

  m68k_trace_op_mnemonic("MOVE.L");
  m68k_trace_op_src("A%d", reg);
  m68k_trace_op_dst("USP");
  if (cpu->status.s) {
    cpu->a[M68K_SP] = m68k_address_reg_value(cpu, reg);
  } else {
    m68k_exception(cpu, mem, M68K_VECTOR_PRIVILEGE_VIOLATION);
  }
}



static void m68k_move_from_usp(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t reg = opcode & 0b111;

  m68k_trace_op_mnemonic("MOVE.L");
  m68k_trace_op_src("USP");
  m68k_trace_op_dst("A%d", reg);
  if (cpu->status.s) {
    m68k_address_reg_set_long(cpu, reg, cpu->a[M68K_SP]);
  } else {
    m68k_exception(cpu, mem, M68K_VECTOR_PRIVILEGE_VIOLATION);
  }
}



static void m68k_moveb(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t value;
  uint8_t src_reg  =  opcode       & 0b111;
  uint8_t src_mode = (opcode >> 3) & 0b111;
  uint8_t dst_mode = (opcode >> 6) & 0b111;
  uint8_t dst_reg  = (opcode >> 9) & 0b111;

  m68k_trace_op_mnemonic("MOVE.B");
  m68k_src_set(cpu, mem, src_reg, src_mode, 1);
  value = m68k_src_read_byte(cpu, mem);
  cpu->status.n = value >> 7;
  cpu->status.z = value == 0;
  cpu->status.v = 0;
  cpu->status.c = 0;
  m68k_dst_set(cpu, mem, dst_reg, dst_mode, 1);
  m68k_dst_write_byte(cpu, mem, value);
}



static void m68k_movew(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint16_t value;
  uint8_t src_reg  =  opcode       & 0b111;
  uint8_t src_mode = (opcode >> 3) & 0b111;
  uint8_t dst_mode = (opcode >> 6) & 0b111;
  uint8_t dst_reg  = (opcode >> 9) & 0b111;

  m68k_src_set(cpu, mem, src_reg, src_mode, 2);
  value = m68k_src_read_word(cpu, mem);
  if (dst_mode == EA_MODE_AR_DIRECT) {
    m68k_trace_op_mnemonic("MOVEA.W");
    m68k_dst_set(cpu, mem, dst_reg, dst_mode, 2);
    m68k_dst_write_long(cpu, mem, (int16_t)value);
  } else {
    m68k_trace_op_mnemonic("MOVE.W");
    cpu->status.n = value >> 15;
    cpu->status.z = value == 0;
    cpu->status.v = 0;
    cpu->status.c = 0;
    m68k_dst_set(cpu, mem, dst_reg, dst_mode, 2);
    m68k_dst_write_word(cpu, mem, value);
  }
}



static void m68k_movel(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint8_t src_reg  =  opcode       & 0b111;
  uint8_t src_mode = (opcode >> 3) & 0b111;
  uint8_t dst_mode = (opcode >> 6) & 0b111;
  uint8_t dst_reg  = (opcode >> 9) & 0b111;

  m68k_src_set(cpu, mem, src_reg, src_mode, 4);
  value = m68k_src_read_long(cpu, mem);
  if (dst_mode == EA_MODE_AR_DIRECT) {
    m68k_trace_op_mnemonic("MOVEA.L");
  } else {
    m68k_trace_op_mnemonic("MOVE.L");
    cpu->status.n = value >> 31;
    cpu->status.z = value == 0;
    cpu->status.v = 0;
    cpu->status.c = 0;
  }
  m68k_dst_set(cpu, mem, dst_reg, dst_mode, 4);
  m68k_dst_write_long(cpu, mem, value);
}



static void m68k_movep(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t address;
  uint8_t areg   =  opcode       & 0b111;
  uint8_t opmode = (opcode >> 6) & 0b111;
  uint8_t dreg   = (opcode >> 9) & 0b111;

  address = m68k_address_reg_value(cpu, areg);
  address += (int16_t)m68k_fetch(cpu, mem);

  switch (opmode) {
  case 0b100:
    m68k_trace_op_mnemonic("MOVEP.W");
    m68k_trace_op_src("(d16, A%d)", areg);
    m68k_trace_op_dst("D%d", dreg);
    cpu->d[dreg] &= ~0xFFFF;
    cpu->d[dreg] |=  mem_read_byte(mem, address + 2);
    cpu->d[dreg] |= (mem_read_byte(mem, address) << 8);
    break;

  case 0b101:
    m68k_trace_op_mnemonic("MOVEP.L");
    m68k_trace_op_src("(d16, A%d)", areg);
    m68k_trace_op_dst("D%d", dreg);
    cpu->d[dreg] =   mem_read_byte(mem, address + 6);
    cpu->d[dreg] |= (mem_read_byte(mem, address + 4) << 8);
    cpu->d[dreg] |= (mem_read_byte(mem, address + 2) << 16);
    cpu->d[dreg] |= (mem_read_byte(mem, address)     << 24);
    break;

  case 0b110:
    m68k_trace_op_mnemonic("MOVEP.W");
    m68k_trace_op_src("D%d", dreg);
    m68k_trace_op_dst("(d16, A%d)", areg);
    mem_write_byte(mem, address + 2, cpu->d[dreg]       & 0xFF);
    mem_write_byte(mem, address,    (cpu->d[dreg] >> 8) & 0xFF);
    break;

  case 0b111:
    m68k_trace_op_mnemonic("MOVEP.L");
    m68k_trace_op_src("D%d", dreg);
    m68k_trace_op_dst("(d16, A%d)", areg);
    mem_write_byte(mem, address + 6,  cpu->d[dreg]        & 0xFF);
    mem_write_byte(mem, address + 4, (cpu->d[dreg] >> 8)  & 0xFF);
    mem_write_byte(mem, address + 2, (cpu->d[dreg] >> 16) & 0xFF);
    mem_write_byte(mem, address,     (cpu->d[dreg] >> 24) & 0xFF);
    break;

  default:
    /* Unhandled MOVEP opmode. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }
}



static void m68k_moveq(m68k_t *cpu, uint16_t opcode)
{
  int8_t value = (int8_t)(opcode & 0xFF);
  uint8_t reg = (opcode >> 9) & 0b111;

  m68k_trace_op_mnemonic("MOVEQ");
  m68k_trace_op_src("%d", value);
  m68k_trace_op_dst("D%d", reg);
  cpu->d[reg] = value;
  cpu->status.n = value >> 7;
  cpu->status.z = value == 0;
  cpu->status.v = 0;
  cpu->status.c = 0;
}



static void m68k_movem_reg_to_mem_word(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  int i;
  uint16_t reg_list_mask;
  uint32_t reg_val;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;

  reg_list_mask = m68k_fetch(cpu, mem);

  m68k_trace_op_mnemonic("MOVEM.W");
  m68k_trace_op_src("*");
  m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);

  if (ea_mode == EA_MODE_AR_PRE_DEC) {
    reg_val = m68k_address_reg_value(cpu, ea_reg);
    m68k_address_reg_inc(cpu, ea_reg, 2);
    if (reg_list_mask & 1) {
      m68k_dst_write_word(cpu, mem, m68k_address_reg_value(cpu, M68K_SP));
      cpu->dst.n -= 2;
      reg_val -= 2;
    }
    for (i = 1; i < 8; i++) {
      if ((reg_list_mask >> i) & 1) {
        m68k_dst_write_word(cpu, mem, cpu->a[7 - i]);
        cpu->dst.n -= 2;
        reg_val -= 2;
      }
    }
    for (i = 8; i < 16; i++) {
      if ((reg_list_mask >> i) & 1) {
        m68k_dst_write_word(cpu, mem, cpu->d[15 - i]);
        cpu->dst.n -= 2;
        reg_val -= 2;
      }
    }
    m68k_address_reg_set_long(cpu, ea_reg, reg_val + 2);

  } else {
    for (i = 0; i < 8; i++) {
      if ((reg_list_mask >> i) & 1) {
        m68k_dst_write_word(cpu, mem, cpu->d[i]);
        cpu->dst.n += 2;
      }
    }
    for (i = 8; i < 15; i++) {
      if ((reg_list_mask >> i) & 1) {
        m68k_dst_write_word(cpu, mem, cpu->a[i - 8]);
        cpu->dst.n += 2;
      }
    }
    if ((reg_list_mask >> 15) & 1) {
      m68k_dst_write_word(cpu, mem, m68k_address_reg_value(cpu, M68K_SP));
    }
  }
}



static void m68k_movem_mem_to_reg_word(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  int i;
  uint16_t reg_list_mask;
  uint32_t reg_val;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;

  reg_list_mask = m68k_fetch(cpu, mem);

  m68k_trace_op_mnemonic("MOVEM.W");
  m68k_trace_op_dst("*");
  m68k_src_set(cpu, mem, ea_reg, ea_mode, 2);

  if (ea_mode == EA_MODE_AR_POST_INC) {
    reg_val = m68k_address_reg_value(cpu, ea_reg);
  }
  for (i = 0; i < 8; i++) {
    if ((reg_list_mask >> i) & 1) {
      cpu->d[i] = (int16_t)m68k_src_read_word(cpu, mem);
      cpu->src.n += 2;
      reg_val += 2;
    }
  }
  for (i = 8; i < 15; i++) {
    if ((reg_list_mask >> i) & 1) {
      cpu->a[i - 8] = (int16_t)m68k_src_read_word(cpu, mem);
      cpu->src.n += 2;
      reg_val += 2;
    }
  }
  if ((reg_list_mask >> 15) & 1) {
    m68k_address_reg_set_long(cpu, M68K_SP,
      (int16_t)m68k_src_read_word(cpu, mem));
    reg_val += 2;
  }
  if (ea_mode == EA_MODE_AR_POST_INC) {
    m68k_address_reg_set_long(cpu, ea_reg, reg_val - 2);
  }
}



static void m68k_movem_reg_to_mem_long(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  int i;
  uint16_t reg_list_mask;
  uint32_t reg_val;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;

  reg_list_mask = m68k_fetch(cpu, mem);

  m68k_trace_op_mnemonic("MOVEM.L");
  m68k_trace_op_src("*");
  m68k_dst_set(cpu, mem, ea_reg, ea_mode, 4);

  if (ea_mode == EA_MODE_AR_PRE_DEC) {
    reg_val = m68k_address_reg_value(cpu, ea_reg);
    m68k_address_reg_inc(cpu, ea_reg, 4);
    if (reg_list_mask & 1) {
      m68k_dst_write_long(cpu, mem, m68k_address_reg_value(cpu, M68K_SP));
      cpu->dst.n -= 4;
      reg_val -= 4;
    }
    for (i = 1; i < 8; i++) {
      if ((reg_list_mask >> i) & 1) {
        m68k_dst_write_long(cpu, mem, cpu->a[7 - i]);
        cpu->dst.n -= 4;
        reg_val -= 4;
      }
    }
    for (i = 8; i < 16; i++) {
      if ((reg_list_mask >> i) & 1) {
        m68k_dst_write_long(cpu, mem, cpu->d[15 - i]);
        cpu->dst.n -= 4;
        reg_val -= 4;
      }
    }
    m68k_address_reg_set_long(cpu, ea_reg, reg_val + 4);

  } else {
    for (i = 0; i < 8; i++) {
      if ((reg_list_mask >> i) & 1) {
        m68k_dst_write_long(cpu, mem, cpu->d[i]);
        cpu->dst.n += 4;
      }
    }
    for (i = 8; i < 15; i++) {
      if ((reg_list_mask >> i) & 1) {
        m68k_dst_write_long(cpu, mem, cpu->a[i - 8]);
        cpu->dst.n += 4;
      }
    }
    if ((reg_list_mask >> 15) & 1) {
      m68k_dst_write_long(cpu, mem, m68k_address_reg_value(cpu, M68K_SP));
    }
  }
}



static void m68k_movem_mem_to_reg_long(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  int i;
  uint16_t reg_list_mask;
  uint32_t reg_val;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;

  reg_list_mask = m68k_fetch(cpu, mem);

  m68k_trace_op_mnemonic("MOVEM.L");
  m68k_trace_op_dst("*");
  m68k_src_set(cpu, mem, ea_reg, ea_mode, 4);

  if (ea_mode == EA_MODE_AR_POST_INC) {
    reg_val = m68k_address_reg_value(cpu, ea_reg);
  }
  for (i = 0; i < 8; i++) {
    if ((reg_list_mask >> i) & 1) {
      cpu->d[i] = m68k_src_read_long(cpu, mem);
      cpu->src.n += 4;
      reg_val += 4;
    }
  }
  for (i = 8; i < 15; i++) {
    if ((reg_list_mask >> i) & 1) {
      cpu->a[i - 8] = m68k_src_read_long(cpu, mem);
      cpu->src.n += 4;
      reg_val += 4;
    }
  }
  if ((reg_list_mask >> 15) & 1) {
    m68k_address_reg_set_long(cpu, M68K_SP, m68k_src_read_long(cpu, mem));
    reg_val += 4;
  }
  if (ea_mode == EA_MODE_AR_POST_INC) {
    m68k_address_reg_set_long(cpu, ea_reg, reg_val - 4);
  }
}



static void m68k_nbcd(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;

  m68k_trace_op_mnemonic("NBCD");
  m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
  m68k_dst_write_byte(cpu, mem,
    m68k_sub_bcd(cpu,
      m68k_dst_read_byte(cpu, mem), 0));
}



static void m68k_neg(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t size    = (opcode >> 6) & 0b11;

  switch (size) {
  case 0b00:
    m68k_trace_op_mnemonic("NEG.B");
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
    m68k_dst_write_byte(cpu, mem,
      m68k_neg_byte(cpu,
        m68k_dst_read_byte(cpu, mem)));
    break;

  case 0b01:
    m68k_trace_op_mnemonic("NEG.W");
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
    m68k_dst_write_word(cpu, mem,
      m68k_neg_word(cpu,
        m68k_dst_read_word(cpu, mem)));
    break;

  case 0b10:
    m68k_trace_op_mnemonic("NEG.L");
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 4);
    m68k_dst_write_long(cpu, mem,
      m68k_neg_long(cpu,
        m68k_dst_read_long(cpu, mem)));
    break;
  }
}



static void m68k_negx(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t size    = (opcode >> 6) & 0b11;

  switch (size) {
  case 0b00:
    m68k_trace_op_mnemonic("NEGX.B");
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
    m68k_dst_write_byte(cpu, mem,
      m68k_subx_byte(cpu,
        m68k_dst_read_byte(cpu, mem), 0));
    break;

  case 0b01:
    m68k_trace_op_mnemonic("NEGX.W");
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
    m68k_dst_write_word(cpu, mem,
      m68k_subx_word(cpu,
        m68k_dst_read_word(cpu, mem), 0));
    break;

  case 0b10:
    m68k_trace_op_mnemonic("NEGX.L");
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 4);
    m68k_dst_write_long(cpu, mem,
      m68k_subx_long(cpu,
        m68k_dst_read_long(cpu, mem), 0));
    break;
  }
}



static void m68k_nop(void)
{
  m68k_trace_op_mnemonic("NOP");
}



static void m68k_not(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t size    = (opcode >> 6) & 0b11;

  switch (size) {
  case 0b00:
    m68k_trace_op_mnemonic("NOT.B");
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
    m68k_dst_write_byte(cpu, mem,
      m68k_not_byte(cpu,
        m68k_dst_read_byte(cpu, mem)));
    break;

  case 0b01:
    m68k_trace_op_mnemonic("NOT.W");
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
    m68k_dst_write_word(cpu, mem,
      m68k_not_word(cpu,
        m68k_dst_read_word(cpu, mem)));
    break;

  case 0b10:
    m68k_trace_op_mnemonic("NOT.L");
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 4);
    m68k_dst_write_long(cpu, mem,
      m68k_not_long(cpu,
        m68k_dst_read_long(cpu, mem)));
    break;
  }
}



static void m68k_sbcd(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t address;
  uint32_t dst_value;
  uint32_t src_value;
  uint8_t reg_y =  opcode       & 0b111;
  uint8_t rm    = (opcode >> 3) & 0b1;
  uint8_t reg_x = (opcode >> 9) & 0b111;

  m68k_trace_op_mnemonic("SBCD");

  if (rm) { /* -(Ay), -(Ax) */
    m68k_trace_op_src("-(A%d)", reg_y);
    m68k_trace_op_dst("-(A%d)", reg_x);
    m68k_address_reg_dec(cpu, reg_y, 1);
    address = m68k_address_reg_value(cpu, reg_y);
    src_value = mem_read_byte(mem, address);
    m68k_address_reg_dec(cpu, reg_x, 1);
    address = m68k_address_reg_value(cpu, reg_x);
    dst_value = mem_read_byte(mem, address);
    dst_value = m68k_sub_bcd(cpu, src_value, dst_value);
    mem_write_byte(mem, address, dst_value);

  } else { /* Dy, Dx */
    m68k_trace_op_src("D%d", reg_y);
    m68k_trace_op_dst("D%d", reg_x);
    dst_value = m68k_sub_bcd(cpu, cpu->d[reg_y], cpu->d[reg_x]);
    cpu->d[reg_x] &= ~0xFF;
    cpu->d[reg_x] |= dst_value;
  }
}



static void m68k_divs(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  int32_t dividend;
  int16_t divisor;
  int32_t quotient;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t reg     = (opcode >> 9) & 0b111;

  m68k_trace_op_mnemonic("DIVS");
  m68k_trace_op_dst("D%d", reg);
  m68k_src_set(cpu, mem, ea_reg, ea_mode, 2);
  dividend = (int32_t)cpu->d[reg];
  divisor = (int16_t)m68k_src_read_word(cpu, mem);
  if (divisor == 0) {
    m68k_exception(cpu, mem, M68K_VECTOR_DIVIDE_BY_ZERO);
  }
  quotient = (int32_t)(dividend / divisor);
  if ((uint32_t)quotient > 0x7FFF && (uint32_t)quotient < 0xFFFF8000) {
    cpu->status.n = 1;
    cpu->status.z = 0;
    cpu->status.v = 1;
    cpu->status.c = 0;
    return; /* Overflow */
  }
  cpu->d[reg] = (dividend % divisor) << 16;
  cpu->d[reg] |= (quotient & 0xFFFF);
  cpu->status.n = (quotient >> 15) & 1;
  cpu->status.z = quotient == 0;
  cpu->status.v = 0;
  cpu->status.c = 0;
}



static void m68k_divu(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t dividend;
  uint16_t divisor;
  uint32_t quotient;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t reg     = (opcode >> 9) & 0b111;

  m68k_trace_op_mnemonic("DIVU");
  m68k_trace_op_dst("D%d", reg);
  m68k_src_set(cpu, mem, ea_reg, ea_mode, 2);
  dividend = cpu->d[reg];
  divisor = m68k_src_read_word(cpu, mem);
  if (divisor == 0) {
    m68k_exception(cpu, mem, M68K_VECTOR_DIVIDE_BY_ZERO);
  }
  quotient = dividend / divisor;
  if (quotient > 0xFFFF) {
    cpu->status.n = 1;
    cpu->status.z = 0;
    cpu->status.v = 1;
    cpu->status.c = 0;
    return; /* Overflow */
  }
  cpu->d[reg] = (dividend % divisor) << 16;
  cpu->d[reg] |= (quotient & 0xFFFF);
  cpu->status.n = (quotient >> 15) & 1;
  cpu->status.z = quotient == 0;
  cpu->status.v = 0;
  cpu->status.c = 0;
}



static void m68k_or(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t op_mode = (opcode >> 6) & 0b111;
  uint8_t reg     = (opcode >> 9) & 0b111;

  switch (op_mode) {
  case 0b000: /* Byte, <ea> & Dn -> Dn */
    m68k_trace_op_mnemonic("OR.B");
    m68k_trace_op_dst("D%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 1);
    value = m68k_or_byte(cpu, m68k_src_read_byte(cpu, mem), cpu->d[reg]);
    cpu->d[reg] &= ~0xFF;
    cpu->d[reg] |= value;
    break;

  case 0b001: /* Word, <ea> & Dn -> Dn */
    m68k_trace_op_mnemonic("OR.W");
    m68k_trace_op_dst("D%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 2);
    value = m68k_or_word(cpu,
      m68k_src_read_word(cpu, mem), cpu->d[reg]);
    cpu->d[reg] &= ~0xFFFF;
    cpu->d[reg] |= value;
    break;

  case 0b010: /* Long, <ea> & Dn -> Dn */
    m68k_trace_op_mnemonic("OR.L");
    m68k_trace_op_dst("D%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 4);
    value = m68k_or_long(cpu,
      m68k_src_read_long(cpu, mem), cpu->d[reg]);
    cpu->d[reg] = value;
    break;

  case 0b011:
    m68k_divu(cpu, mem, opcode);
    break;

  case 0b100: /* Byte, Dn & <ea> -> <ea> */
    if (ea_mode == EA_MODE_DR_DIRECT || ea_mode == EA_MODE_AR_DIRECT) {
      m68k_sbcd(cpu, mem, opcode);
    } else {
      m68k_trace_op_mnemonic("OR.B");
      m68k_trace_op_src("D%d", reg);
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
      m68k_dst_write_byte(cpu, mem,
        m68k_or_byte(cpu, cpu->d[reg],
          m68k_dst_read_byte(cpu, mem)));
    }
    break;

  case 0b101: /* Word, Dn & <ea> -> <ea> */
    if (ea_mode == EA_MODE_AR_DIRECT) {
      /* Unhandled OR sub-instruction. */
      m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    } else {
      m68k_trace_op_mnemonic("OR.W");
      m68k_trace_op_src("D%d", reg);
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
      m68k_dst_write_word(cpu, mem,
        m68k_or_word(cpu, cpu->d[reg],
          m68k_dst_read_word(cpu, mem)));
    }
    break;

  case 0b110: /* Long, Dn & <ea> -> <ea> */
    if (ea_mode == EA_MODE_AR_DIRECT) {
      /* Unhandled OR sub-instruction. */
      m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    } else {
      m68k_trace_op_mnemonic("OR.L");
      m68k_trace_op_src("D%d", reg);
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 4);
      m68k_dst_write_long(cpu, mem,
        m68k_or_long(cpu, cpu->d[reg],
          m68k_dst_read_long(cpu, mem)));
    }
    break;

  case 0b111:
    m68k_divs(cpu, mem, opcode);
    break;
  }
}



static void m68k_ori(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t size    = (opcode >> 6) & 0b11;

  switch (size) {
  case 0b00:
    m68k_trace_op_mnemonic("ORI.B");
    value = m68k_fetch(cpu, mem) & 0xFF;
    m68k_trace_op_src("#$%02x", value);
    if ((ea_mode == EA_MODE_EXT) && (ea_reg == EA_MODE_EXT_IMMEDIATE)) {
      m68k_trace_op_dst("CCR");
      cpu->sr |= (value & 0x1F);
    } else {
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
      m68k_dst_write_byte(cpu, mem,
        m68k_or_byte(cpu, value,
          m68k_dst_read_byte(cpu, mem)));
    }
    break;

  case 0b01:
    m68k_trace_op_mnemonic("ORI.W");
    value = m68k_fetch(cpu, mem);
    m68k_trace_op_src("#$%04x", value);
    if ((ea_mode == EA_MODE_EXT) && (ea_reg == EA_MODE_EXT_IMMEDIATE)) {
      m68k_trace_op_dst("SR");
      if (cpu->status.s) {
        cpu->sr |= m68k_sr_filter_bits(value);
      } else {
        m68k_exception(cpu, mem, M68K_VECTOR_PRIVILEGE_VIOLATION);
      }
    } else {
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
      m68k_dst_write_word(cpu, mem,
        m68k_or_word(cpu, value,
          m68k_dst_read_word(cpu, mem)));
    }
    break;

  case 0b10:
    m68k_trace_op_mnemonic("ORI.L");
    value = m68k_fetch(cpu, mem) << 16;
    value |= m68k_fetch(cpu, mem);
    m68k_trace_op_src("#$%08x", value);
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 4);
    m68k_dst_write_long(cpu, mem,
      m68k_or_long(cpu, value,
        m68k_dst_read_long(cpu, mem)));
    break;

  case 0b11:
    /* Unhandled ORI size. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }
}



static void m68k_pea(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;

  m68k_trace_op_mnemonic("PEA");
  m68k_src_set(cpu, mem, ea_reg, ea_mode, 4);
  m68k_stack_push(cpu, mem, cpu->src.n % 0x10000);
  m68k_stack_push(cpu, mem, cpu->src.n / 0x10000);
}



static void m68k_reset(m68k_t *cpu, mem_t *mem)
{
  m68k_trace_op_mnemonic("RESET");
  if (cpu->status.s == false) {
    m68k_exception(cpu, mem, M68K_VECTOR_PRIVILEGE_VIOLATION);
  }
}



static void m68k_ro_reg_byte(m68k_t *cpu, uint16_t opcode)
{
  uint8_t value;
  uint8_t reg   =  opcode       & 0b111;
  uint8_t ir    = (opcode >> 5) & 0b1;
  uint8_t dr    = (opcode >> 8) & 0b1;
  uint8_t count = (opcode >> 9) & 0b111;

  m68k_trace_op_dst("D%d", reg);
  if (ir) {
    m68k_trace_op_src("D%d", count);
    count = cpu->d[count] % 64;
  } else {
    m68k_trace_op_src("#%d", count);
    if (count == 0) {
      count = 8;
    }
  }
  if (dr) {
    m68k_trace_op_mnemonic("ROL.B");
    value = m68k_rol_byte(cpu, cpu->d[reg] & 0xFF, count);
  } else {
    m68k_trace_op_mnemonic("ROR.B");
    value = m68k_ror_byte(cpu, cpu->d[reg] & 0xFF, count);
  }
  cpu->d[reg] &= ~0xFF;
  cpu->d[reg] |= value;
}



static void m68k_ro_reg_word(m68k_t *cpu, uint16_t opcode)
{
  uint16_t value;
  uint8_t reg   =  opcode       & 0b111;
  uint8_t ir    = (opcode >> 5) & 0b1;
  uint8_t dr    = (opcode >> 8) & 0b1;
  uint8_t count = (opcode >> 9) & 0b111;

  m68k_trace_op_dst("D%d", reg);
  if (ir) {
    m68k_trace_op_src("D%d", count);
    count = cpu->d[count] % 64;
  } else {
    m68k_trace_op_src("#%d", count);
    if (count == 0) {
      count = 8;
    }
  }
  if (dr) {
    m68k_trace_op_mnemonic("ROL.W");
    value = m68k_rol_word(cpu, cpu->d[reg] & 0xFFFF, count);
  } else {
    m68k_trace_op_mnemonic("ROR.W");
    value = m68k_ror_word(cpu, cpu->d[reg] & 0xFFFF, count);
  }
  cpu->d[reg] &= ~0xFFFF;
  cpu->d[reg] |= value;
}



static void m68k_ro_reg_long(m68k_t *cpu, uint16_t opcode)
{
  uint8_t reg   =  opcode       & 0b111;
  uint8_t ir    = (opcode >> 5) & 0b1;
  uint8_t dr    = (opcode >> 8) & 0b1;
  uint8_t count = (opcode >> 9) & 0b111;

  m68k_trace_op_dst("D%d", reg);
  if (ir) {
    m68k_trace_op_src("D%d", count);
    count = cpu->d[count] % 64;
  } else {
    m68k_trace_op_src("#%d", count);
    if (count == 0) {
      count = 8;
    }
  }
  if (dr) {
    m68k_trace_op_mnemonic("ROL.L");
    cpu->d[reg] = m68k_rol_long(cpu, cpu->d[reg], count);
  } else {
    m68k_trace_op_mnemonic("ROR.L");
    cpu->d[reg] = m68k_ror_long(cpu, cpu->d[reg], count);
  }
}



static void m68k_ro_mem(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t dr      = (opcode >> 8) & 0b1;

  m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
  if (dr) {
    m68k_trace_op_mnemonic("ROL.W");
    m68k_dst_write_word(cpu, mem,
      m68k_rol_word(cpu,
        m68k_dst_read_word(cpu, mem), 1));
  } else {
    m68k_trace_op_mnemonic("ROR.W");
    m68k_dst_write_word(cpu, mem,
      m68k_ror_word(cpu,
        m68k_dst_read_word(cpu, mem), 1));
  }
}



static void m68k_rox_reg_byte(m68k_t *cpu, uint16_t opcode)
{
  uint8_t value;
  uint8_t reg   =  opcode       & 0b111;
  uint8_t ir    = (opcode >> 5) & 0b1;
  uint8_t dr    = (opcode >> 8) & 0b1;
  uint8_t count = (opcode >> 9) & 0b111;

  m68k_trace_op_dst("D%d", reg);
  if (ir) {
    m68k_trace_op_src("D%d", count);
    count = cpu->d[count] % 64;
  } else {
    m68k_trace_op_src("#%d", count);
    if (count == 0) {
      count = 8;
    }
  }
  if (dr) {
    m68k_trace_op_mnemonic("ROXL.B");
    value = m68k_roxl_byte(cpu, cpu->d[reg] & 0xFF, count);
  } else {
    m68k_trace_op_mnemonic("ROXR.B");
    value = m68k_roxr_byte(cpu, cpu->d[reg] & 0xFF, count);
  }
  cpu->d[reg] &= ~0xFF;
  cpu->d[reg] |= value;
}



static void m68k_rox_reg_word(m68k_t *cpu, uint16_t opcode)
{
  uint16_t value;
  uint8_t reg   =  opcode       & 0b111;
  uint8_t ir    = (opcode >> 5) & 0b1;
  uint8_t dr    = (opcode >> 8) & 0b1;
  uint8_t count = (opcode >> 9) & 0b111;

  m68k_trace_op_dst("D%d", reg);
  if (ir) {
    m68k_trace_op_src("D%d", count);
    count = cpu->d[count] % 64;
  } else {
    m68k_trace_op_src("#%d", count);
    if (count == 0) {
      count = 8;
    }
  }
  if (dr) {
    m68k_trace_op_mnemonic("ROXL.W");
    value = m68k_roxl_word(cpu, cpu->d[reg] & 0xFFFF, count);
  } else {
    m68k_trace_op_mnemonic("ROXR.W");
    value = m68k_roxr_word(cpu, cpu->d[reg] & 0xFFFF, count);
  }
  cpu->d[reg] &= ~0xFFFF;
  cpu->d[reg] |= value;
}



static void m68k_rox_reg_long(m68k_t *cpu, uint16_t opcode)
{
  uint8_t reg   =  opcode       & 0b111;
  uint8_t ir    = (opcode >> 5) & 0b1;
  uint8_t dr    = (opcode >> 8) & 0b1;
  uint8_t count = (opcode >> 9) & 0b111;

  m68k_trace_op_dst("D%d", reg);
  if (ir) {
    m68k_trace_op_src("D%d", count);
    count = cpu->d[count] % 64;
  } else {
    m68k_trace_op_src("#%d", count);
    if (count == 0) {
      count = 8;
    }
  }
  if (dr) {
    m68k_trace_op_mnemonic("ROXL.L");
    cpu->d[reg] = m68k_roxl_long(cpu, cpu->d[reg], count);
  } else {
    m68k_trace_op_mnemonic("ROXR.L");
    cpu->d[reg] = m68k_roxr_long(cpu, cpu->d[reg], count);
  }
}



static void m68k_rox_mem(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t dr      = (opcode >> 8) & 0b1;

  m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
  if (dr) {
    m68k_trace_op_mnemonic("ROXL.W");
    m68k_dst_write_word(cpu, mem,
      m68k_roxl_word(cpu,
        m68k_dst_read_word(cpu, mem), 1));
  } else {
    m68k_trace_op_mnemonic("ROXR.W");
    m68k_dst_write_word(cpu, mem,
      m68k_roxr_word(cpu,
        m68k_dst_read_word(cpu, mem), 1));
  }
}



static void m68k_rte(m68k_t *cpu, mem_t *mem)
{
  uint16_t new_sr;
  uint32_t old_pc;
  uint32_t bad_address;

  m68k_trace_op_mnemonic("RTE");
  old_pc = cpu->pc;
  if (cpu->status.s) {
    new_sr   = m68k_sr_filter_bits(m68k_stack_pop(cpu, mem));
    cpu->pc  = m68k_stack_pop(cpu, mem) * 0x10000;
    cpu->pc += m68k_stack_pop(cpu, mem);
    cpu->sr  = new_sr;
  } else {
    m68k_exception(cpu, mem, M68K_VECTOR_PRIVILEGE_VIOLATION);
  }
  if (cpu->pc % 2 != 0) {
    bad_address = cpu->pc;
    cpu->pc = old_pc;
    m68k_address_error(cpu, mem, bad_address, true, true);
  }
}



static void m68k_rtr(m68k_t *cpu, mem_t *mem)
{
  uint16_t value;
  uint32_t old_pc;
  uint32_t bad_address;

  m68k_trace_op_mnemonic("RTR");
  old_pc = cpu->pc;
  value = m68k_stack_pop(cpu, mem);
  cpu->sr &= ~0x1F;
  cpu->sr |= (value & 0x1F);
  cpu->pc  = m68k_stack_pop(cpu, mem) * 0x10000;
  cpu->pc += m68k_stack_pop(cpu, mem);
  if (cpu->pc % 2 != 0) {
    bad_address = cpu->pc;
    cpu->pc = old_pc;
    m68k_address_error(cpu, mem, bad_address, true, true);
  }
}



static void m68k_rts(m68k_t *cpu, mem_t *mem)
{
  uint32_t old_pc;
  uint32_t bad_address;

  m68k_trace_op_mnemonic("RTS");
  old_pc = cpu->pc;
  cpu->pc  = m68k_stack_pop(cpu, mem) * 0x10000;
  cpu->pc += m68k_stack_pop(cpu, mem);
  if (cpu->pc % 2 != 0) {
    bad_address = cpu->pc;
    cpu->pc = old_pc;
    m68k_address_error(cpu, mem, bad_address, true, true);
  }
}



static void m68k_scc(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  bool result = false;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t cond    = (opcode >> 8) & 0b1111;

  m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);

  switch (cond) {
  case 0b0000:
    m68k_trace_op_mnemonic("ST");
    result = true;
    break;

  case 0b0001:
    m68k_trace_op_mnemonic("SF");
    result = false;
    break;

  case 0b0010:
    m68k_trace_op_mnemonic("SHI");
    if (cpu->status.c == 0 && cpu->status.z == 0) {
      result = true;
    }
    break;

  case 0b0011:
    m68k_trace_op_mnemonic("SLS");
    if (cpu->status.c == 1 || cpu->status.z == 1) {
      result = true;
    }
    break;

  case 0b0100:
    m68k_trace_op_mnemonic("SCC");
    if (cpu->status.c == 0) {
      result = true;
    }
    break;

  case 0b0101:
    m68k_trace_op_mnemonic("SCS");
    if (cpu->status.c == 1) {
      result = true;
    }
    break;

  case 0b0110:
    m68k_trace_op_mnemonic("SNE");
    if (cpu->status.z == 0) {
      result = true;
    }
    break;

  case 0b0111:
    m68k_trace_op_mnemonic("SEQ");
    if (cpu->status.z == 1) {
      result = true;
    }
    break;

  case 0b1000:
    m68k_trace_op_mnemonic("SVC");
    if (cpu->status.v == 0) {
      result = true;
    }
    break;

  case 0b1001:
    m68k_trace_op_mnemonic("SVS");
    if (cpu->status.v == 1) {
      result = true;
    }
    break;

  case 0b1010:
    m68k_trace_op_mnemonic("SPL");
    if (cpu->status.n == 0) {
      result = true;
    }
    break;

  case 0b1011:
    m68k_trace_op_mnemonic("SMI");
    if (cpu->status.n == 1) {
      result = true;
    }
    break;

  case 0b1100:
    m68k_trace_op_mnemonic("SGE");
    if ((cpu->status.n == 1 && cpu->status.v == 1) ||
        (cpu->status.n == 0 && cpu->status.v == 0))
    {
      result = true;
    }
    break;

  case 0b1101:
    m68k_trace_op_mnemonic("SLT");
    if ((cpu->status.n == 0 && cpu->status.v == 1) ||
        (cpu->status.n == 1 && cpu->status.v == 0))
    {
      result = true;
    }
    break;

  case 0b1110:
    m68k_trace_op_mnemonic("SGT");
    if ((cpu->status.n == 0 && cpu->status.v == 0 && cpu->status.z == 0) ||
        (cpu->status.n == 1 && cpu->status.v == 1 && cpu->status.z == 0))
    {
      result = true;
    }
    break;

  case 0b1111:
    m68k_trace_op_mnemonic("SLE");
    if ((cpu->status.z == 1) ||
        (cpu->status.n == 1 && cpu->status.v == 0) ||
        (cpu->status.n == 0 && cpu->status.v == 1))
    {
      result = true;
    }
    break;
  }

  if (result == true) {
    m68k_dst_write_byte(cpu, mem, 0xFF);
  } else {
    m68k_dst_write_byte(cpu, mem, 0x00);
  }
}



static void m68k_stop(m68k_t *cpu, mem_t *mem)
{
  m68k_trace_op_mnemonic("STOP");
  if (cpu->status.s) {
    cpu->sr = m68k_sr_filter_bits(m68k_fetch(cpu, mem));
    cpu->pc -= 4;
  } else {
    m68k_exception(cpu, mem, M68K_VECTOR_PRIVILEGE_VIOLATION);
  }
}



static void m68k_subx(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  bool error = false;
  uint32_t address;
  uint32_t dst_value;
  uint32_t src_value;
  uint8_t reg_y =  opcode       & 0b111;
  uint8_t rm    = (opcode >> 3) & 0b1;
  uint8_t size  = (opcode >> 6) & 0b11;
  uint8_t reg_x = (opcode >> 9) & 0b111;

  switch (size) {
  case 0b00:
    m68k_trace_op_mnemonic("SUBX.B");
    if (rm) { /* -(Ay), -(Ax) */
      m68k_trace_op_src("-(A%d)", reg_y);
      m68k_trace_op_dst("-(A%d)", reg_x);
      m68k_address_reg_dec(cpu, reg_y, 1);
      address = m68k_address_reg_value(cpu, reg_y);
      src_value = mem_read_byte(mem, address);
      m68k_address_reg_dec(cpu, reg_x, 1);
      address = m68k_address_reg_value(cpu, reg_x);
      dst_value = mem_read_byte(mem, address);
      dst_value = m68k_subx_byte(cpu, src_value, dst_value);
      mem_write_byte(mem, address, dst_value);

    } else { /* Dy, Dx */
      m68k_trace_op_src("D%d", reg_y);
      m68k_trace_op_dst("D%d", reg_x);
      dst_value = m68k_subx_byte(cpu, cpu->d[reg_y], cpu->d[reg_x]);
      cpu->d[reg_x] &= ~0xFF;
      cpu->d[reg_x] |= dst_value;
    }
    break;

  case 0b01:
    m68k_trace_op_mnemonic("SUBX.W");
    if (rm) { /* -(Ay), -(Ax) */
      m68k_trace_op_src("-(A%d)", reg_y);
      m68k_trace_op_dst("-(A%d)", reg_x);
      m68k_address_reg_dec(cpu, reg_y, 2);
      address = m68k_address_reg_value(cpu, reg_y);
      src_value = mem_read_word(mem, address, &error);
      if (error) {
        m68k_address_error(cpu, mem, address, true, false);
      }
      m68k_address_reg_dec(cpu, reg_x, 2);
      address = m68k_address_reg_value(cpu, reg_x);
      dst_value = mem_read_word(mem, address, &error);
      if (error) {
        m68k_address_error(cpu, mem, address, true, false);
      }
      dst_value = m68k_subx_word(cpu, src_value, dst_value);
      mem_write_word(mem, address, dst_value, &error);

    } else { /* Dy, Dx */
      m68k_trace_op_src("D%d", reg_y);
      m68k_trace_op_dst("D%d", reg_x);
      dst_value = m68k_subx_word(cpu, cpu->d[reg_y], cpu->d[reg_x]);
      cpu->d[reg_x] &= ~0xFFFF;
      cpu->d[reg_x] |= dst_value;
    }
    break;

  case 0b10:
    m68k_trace_op_mnemonic("SUBX.L");
    if (rm) { /* -(Ay), -(Ax) */
      m68k_trace_op_src("-(A%d)", reg_y);
      m68k_trace_op_dst("-(A%d)", reg_x);
      m68k_address_reg_dec(cpu, reg_y, 4);
      address = m68k_address_reg_value(cpu, reg_y);
      src_value = mem_read_long(mem, address, &error);
      if (error) {
        m68k_address_error(cpu, mem, address, true, false);
      }
      m68k_address_reg_dec(cpu, reg_x, 4);
      address = m68k_address_reg_value(cpu, reg_x);
      dst_value = mem_read_long(mem, address, &error);
      if (error) {
        m68k_address_error(cpu, mem, address, true, false);
      }
      dst_value = m68k_subx_long(cpu, src_value, dst_value);
      mem_write_long(mem, address, dst_value, &error);

    } else { /* Dy, Dx */
      m68k_trace_op_src("D%d", reg_y);
      m68k_trace_op_dst("D%d", reg_x);
      dst_value = m68k_subx_long(cpu, cpu->d[reg_y], cpu->d[reg_x]);
      cpu->d[reg_x] = dst_value;
    }
    break;

  case 0b11:
    /* Unhandled SUBX size. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }
}



static void m68k_sub(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t op_mode = (opcode >> 6) & 0b111;
  uint8_t reg     = (opcode >> 9) & 0b111;

  switch (op_mode) {
  case 0b000: /* Byte, Dn - <ea> -> Dn */
    m68k_trace_op_mnemonic("SUB.B");
    m68k_trace_op_dst("D%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 1);
    value = m68k_sub_byte(cpu, m68k_src_read_byte(cpu, mem), cpu->d[reg]);
    cpu->d[reg] &= ~0xFF;
    cpu->d[reg] |= value;
    break;

  case 0b001: /* Word, Dn - <ea> -> Dn */
    m68k_trace_op_mnemonic("SUB.W");
    m68k_trace_op_dst("D%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 2);
    value = m68k_sub_word(cpu,
      m68k_src_read_word(cpu, mem), cpu->d[reg], false);
    cpu->d[reg] &= ~0xFFFF;
    cpu->d[reg] |= value;
    break;

  case 0b010: /* Long, Dn - <ea> -> Dn */
    m68k_trace_op_mnemonic("SUB.L");
    m68k_trace_op_dst("D%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 4);
    value = m68k_sub_long(cpu,
      m68k_src_read_long(cpu, mem), cpu->d[reg], false);
    cpu->d[reg] = value;
    break;

  case 0b011: /* Word, <ea>, An */
    m68k_trace_op_mnemonic("SUBA.W");
    m68k_trace_op_dst("A%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 2);
    value = m68k_address_reg_value(cpu, reg);
    value = m68k_sub_long(cpu,
      (int16_t)m68k_src_read_word(cpu, mem), value, true);
    m68k_address_reg_set_long(cpu, reg, value);
    break;

  case 0b100: /* Byte, <ea> - Dn -> <ea> */
    if (ea_mode == EA_MODE_DR_DIRECT || ea_mode == EA_MODE_AR_DIRECT) {
      m68k_subx(cpu, mem, opcode);
    } else {
      m68k_trace_op_mnemonic("SUB.B");
      m68k_trace_op_src("D%d", reg);
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
      m68k_dst_write_byte(cpu, mem,
        m68k_sub_byte(cpu, cpu->d[reg],
          m68k_dst_read_byte(cpu, mem)));
    }
    break;

  case 0b101: /* Word, <ea> - Dn -> <ea> */
    if (ea_mode == EA_MODE_DR_DIRECT || ea_mode == EA_MODE_AR_DIRECT) {
      m68k_subx(cpu, mem, opcode);
    } else {
      m68k_trace_op_mnemonic("SUB.W");
      m68k_trace_op_src("D%d", reg);
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
      m68k_dst_write_word(cpu, mem,
        m68k_sub_word(cpu, cpu->d[reg],
          m68k_dst_read_word(cpu, mem), false));
    }
    break;

  case 0b110: /* Long, <ea> - Dn -> <ea> */
    if (ea_mode == EA_MODE_DR_DIRECT || ea_mode == EA_MODE_AR_DIRECT) {
      m68k_subx(cpu, mem, opcode);
    } else {
      m68k_trace_op_mnemonic("SUB.L");
      m68k_trace_op_src("D%d", reg);
      m68k_dst_set(cpu, mem, ea_reg, ea_mode, 4);
      m68k_dst_write_long(cpu, mem,
        m68k_sub_long(cpu, cpu->d[reg],
          m68k_dst_read_long(cpu, mem), false));
    }
    break;

  case 0b111: /* Long, <ea>, An */
    m68k_trace_op_mnemonic("SUBA.L");
    m68k_trace_op_dst("A%d", reg);
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 4);
    value = m68k_address_reg_value(cpu, reg);
    value = m68k_sub_long(cpu,
      m68k_src_read_long(cpu, mem), value, true);
    m68k_address_reg_set_long(cpu, reg, value);
    break;
  }
}



static void m68k_subi(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t size    = (opcode >> 6) & 0b11;

  switch (size) {
  case 0b00:
    m68k_trace_op_mnemonic("SUBI.B");
    value = m68k_fetch(cpu, mem) & 0xFF;
    m68k_trace_op_src("#$%02x", value);
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
    m68k_dst_write_byte(cpu, mem,
      m68k_sub_byte(cpu, value,
        m68k_dst_read_byte(cpu, mem)));
    break;

  case 0b01:
    m68k_trace_op_mnemonic("SUBI.W");
    value = m68k_fetch(cpu, mem);
    m68k_trace_op_src("#$%04x", value);
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
    m68k_dst_write_word(cpu, mem,
      m68k_sub_word(cpu, value,
        m68k_dst_read_word(cpu, mem), false));
    break;

  case 0b10:
    m68k_trace_op_mnemonic("SUBI.L");
    value = m68k_fetch(cpu, mem) << 16;
    value |= m68k_fetch(cpu, mem);
    m68k_trace_op_src("#$%08x", value);
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 4);
    m68k_dst_write_long(cpu, mem,
      m68k_sub_long(cpu, value,
        m68k_dst_read_long(cpu, mem), false));
    break;

  case 0b11:
    /* Unhandled SUBI size. */
    m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
    break;
  }
}



static void m68k_subq(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t size    = (opcode >> 6) & 0b11;
  uint8_t value   = (opcode >> 9) & 0b111;

  if (value == 0) {
    value = 8;
  }

  switch (size) {
  case 0b00:
    m68k_trace_op_mnemonic("SUBQ.B");
    m68k_trace_op_src("%d", value);
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
    m68k_dst_write_byte(cpu, mem,
      m68k_sub_byte(cpu, value,
        m68k_dst_read_byte(cpu, mem)));
    break;

  case 0b01:
    m68k_trace_op_mnemonic("SUBQ.W");
    m68k_trace_op_src("%d", value);
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 2);
    m68k_dst_write_word(cpu, mem,
      m68k_sub_word(cpu, value,
        m68k_dst_read_word(cpu, mem), ea_mode == EA_MODE_AR_DIRECT));
    break;

  case 0b10:
    m68k_trace_op_mnemonic("SUBQ.L");
    m68k_trace_op_src("%d", value);
    m68k_dst_set(cpu, mem, ea_reg, ea_mode, 4);
    m68k_dst_write_long(cpu, mem,
      m68k_sub_long(cpu, value,
        m68k_dst_read_long(cpu, mem), ea_mode == EA_MODE_AR_DIRECT));
    break;
  }
}



static void m68k_swap(m68k_t *cpu, uint16_t opcode)
{
  uint32_t value;
  uint8_t reg = opcode & 0b111;

  m68k_trace_op_mnemonic("SWAP");
  m68k_trace_op_dst("D%d", reg);
  value = cpu->d[reg] >> 16;
  value |= cpu->d[reg] << 16;
  cpu->d[reg] = value;
  cpu->status.n = value >> 31;
  cpu->status.z = value == 0;
  cpu->status.c = 0;
  cpu->status.v = 0;
}



static void m68k_tas(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;

  m68k_trace_op_mnemonic("TAS");
  m68k_dst_set(cpu, mem, ea_reg, ea_mode, 1);
  value = m68k_dst_read_byte(cpu, mem);
  cpu->status.n = value >> 7;
  cpu->status.z = value == 0;
  cpu->status.v = 0;
  cpu->status.c = 0;
  m68k_dst_write_byte(cpu, mem, value | 0x80);
}



static void m68k_trap(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint8_t vector = opcode & 0b1111;

  m68k_trace_op_mnemonic("TRAP");
  m68k_trace_op_dst("%d", vector);
  if (vector == 15 && cpu->trap_15_hook != NULL) {
    (*cpu->trap_15_hook)(cpu->d);
    return;
  }
  cpu->old_pc = cpu->pc; /* To be able to return from exception. */
  m68k_exception(cpu, mem, (vector + 32) * 4);
}



static void m68k_trapv(m68k_t *cpu, mem_t *mem)
{
  m68k_trace_op_mnemonic("TRAPV");
  if (cpu->status.v) {
    cpu->old_pc = cpu->pc; /* To be able to return from exception. */
    m68k_exception(cpu, mem, M68K_VECTOR_TRAPV_INSTRUCTION);
  }
}



static void m68k_tst(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint8_t ea_reg  =  opcode       & 0b111;
  uint8_t ea_mode = (opcode >> 3) & 0b111;
  uint8_t size    = (opcode >> 6) & 0b11;

  switch (size) {
  case 0b00:
    m68k_trace_op_mnemonic("TST.B");
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 1);
    value = m68k_src_read_byte(cpu, mem);
    cpu->status.n = value >> 7;
    break;

  case 0b01:
    m68k_trace_op_mnemonic("TST.W");
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 2);
    value = m68k_src_read_word(cpu, mem);
    cpu->status.n = value >> 15;
    break;

  case 0b10:
    m68k_trace_op_mnemonic("TST.L");
    m68k_src_set(cpu, mem, ea_reg, ea_mode, 4);
    value = m68k_src_read_long(cpu, mem);
    cpu->status.n = value >> 31;
    break;
  }

  cpu->status.z = value == 0;
  cpu->status.v = 0;
  cpu->status.c = 0;
}



static void m68k_unlk(m68k_t *cpu, mem_t *mem, uint16_t opcode)
{
  uint32_t value;
  uint8_t reg = opcode & 0b111;

  m68k_trace_op_mnemonic("UNLK");
  m68k_trace_op_dst("A%d", reg);
  value = m68k_address_reg_value(cpu, reg);
  if (value % 2 != 0) {
    m68k_address_error(cpu, mem, value, true, false);
  }
  m68k_address_reg_set_long(cpu, M68K_SP, value);
  value  = m68k_stack_pop(cpu, mem) * 0x10000;
  value += m68k_stack_pop(cpu, mem);
  m68k_address_reg_set_long(cpu, reg, value);
}



void m68k_execute(m68k_t *cpu, mem_t *mem)
{
  uint16_t opcode;

  if (setjmp(m68k_exception_jmp) > 0) {
    m68k_trace_end();
    return;
  }

  m68k_trace_start(cpu);
  cpu->old_pc = cpu->pc;
  opcode = m68k_fetch(cpu, mem);

  switch (opcode >> 12) {
  case 0b0000: /* Bit Manipulation/MOVEP/Immediate */
    if (((opcode >> 3) & 0x7) == 0b001) {
      m68k_movep(cpu, mem, opcode);
      break;
    }

    switch ((opcode >> 8) & 0xF) {
    case 0b0000:
      m68k_ori(cpu, mem, opcode);
      break;

    case 0b0001:
    case 0b0011:
    case 0b0101:
    case 0b0111:
    case 0b1001:
    case 0b1011:
    case 0b1101:
    case 0b1111:
      switch ((opcode >> 6) & 0x3) {
      case 0b00:
        m68k_btst_reg(cpu, mem, opcode);
        break;

      case 0b01:
        m68k_bchg_reg(cpu, mem, opcode);
        break;

      case 0b10:
        m68k_bclr_reg(cpu, mem, opcode);
        break;

      case 0b11:
        m68k_bset_reg(cpu, mem, opcode);
        break;
      }
      break;

    case 0b0010:
      m68k_andi(cpu, mem, opcode);
      break;

    case 0b0100:
      m68k_subi(cpu, mem, opcode);
      break;

    case 0b0110:
      m68k_addi(cpu, mem, opcode);
      break;

    case 0b1000:
      switch ((opcode >> 6) & 0x3) {
      case 0b00:
        m68k_btst_imm(cpu, mem, opcode);
        break;

      case 0b01:
        m68k_bchg_imm(cpu, mem, opcode);
        break;

      case 0b10:
        m68k_bclr_imm(cpu, mem, opcode);
        break;

      case 0b11:
        m68k_bset_imm(cpu, mem, opcode);
        break;
      }
      break;

    case 0b1010:
      m68k_eori(cpu, mem, opcode);
      break;

    case 0b1100:
      m68k_cmpi(cpu, mem, opcode);
      break;

    default:
      m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
      break;
    }
    break;

  case 0b0001: /* Move Byte */
    m68k_moveb(cpu, mem, opcode);
    break;

  case 0b0010: /* Move Long */
    m68k_movel(cpu, mem, opcode);
    break;

  case 0b0011: /* Move Word */
    m68k_movew(cpu, mem, opcode);
    break;

  case 0b0100: /* Miscellaneous */
    switch ((opcode >> 6) & 0x3F) {
    case 0b000000:
    case 0b000001:
    case 0b000010:
      m68k_negx(cpu, mem, opcode);
      break;

    case 0b001000:
    case 0b001001:
    case 0b001010:
      m68k_clr(cpu, mem, opcode);
      break;

    case 0b000110:
    case 0b001110:
    case 0b010110:
    case 0b011110:
    case 0b100110:
    case 0b101110:
    case 0b110110:
    case 0b111110:
      m68k_chk(cpu, mem, opcode);
      break;

    case 0b000111:
    case 0b001111:
    case 0b010111:
    case 0b011111:
    case 0b100111:
    case 0b101111:
    case 0b110111:
    case 0b111111:
      m68k_lea(cpu, mem, opcode);
      break;

    case 0b010000:
    case 0b010001:
    case 0b010010:
      m68k_neg(cpu, mem, opcode);
      break;

    case 0b011000:
    case 0b011001:
    case 0b011010:
      m68k_not(cpu, mem, opcode);
      break;

    case 0b010011:
      m68k_move_to_ccr(cpu, mem, opcode);
      break;

    case 0b011011:
      m68k_move_to_sr(cpu, mem, opcode);
      break;

    case 0b000011:
      m68k_move_from_sr(cpu, mem, opcode);
      break;

    case 0b100000:
      m68k_nbcd(cpu, mem, opcode);
      break;

    case 0b100001:
      switch ((opcode >> 3) & 0x7) {
      case 0b000:
        m68k_swap(cpu, opcode);
        break;

      case 0b010:
      case 0b101:
      case 0b110:
      case 0b111:
        m68k_pea(cpu, mem, opcode);
        break;

      default:
        m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
        break;
      }
      break;

    case 0b101000:
    case 0b101001:
    case 0b101010:
      m68k_tst(cpu, mem, opcode);
      break;

    case 0b101011:
      m68k_tas(cpu, mem, opcode);
      break;

    case 0b100010:
      if (((opcode >> 3) & 0x7) == 0) {
        m68k_ext(cpu, opcode);
      } else {
        m68k_movem_reg_to_mem_word(cpu, mem, opcode);
      }
      break;

    case 0b100011:
      if (((opcode >> 3) & 0x7) == 0) {
        m68k_ext(cpu, opcode);
      } else {
        m68k_movem_reg_to_mem_long(cpu, mem, opcode);
      }
      break;

    case 0b110010:
      m68k_movem_mem_to_reg_word(cpu, mem, opcode);
      break;

    case 0b110011:
      m68k_movem_mem_to_reg_long(cpu, mem, opcode);
      break;

    case 0b111011:
      m68k_jmp(cpu, mem, opcode);
      break;

    case 0b111010:
      m68k_jsr(cpu, mem, opcode);
      break;

    case 0b111001:
      switch ((opcode >> 3) & 0x7) {
      case 0b000:
      case 0b001:
        m68k_trap(cpu, mem, opcode);
        break;

      case 0b010:
        m68k_link(cpu, mem, opcode);
        break;

      case 0b011:
        m68k_unlk(cpu, mem, opcode);
        break;

      case 0b100:
        m68k_move_to_usp(cpu, mem, opcode);
        break;

      case 0b101:
        m68k_move_from_usp(cpu, mem, opcode);
        break;

      case 0b110:
        switch (opcode & 0x7) {
        case 0b000:
          m68k_reset(cpu, mem);
          break;

        case 0b001:
          m68k_nop();
          break;

        case 0b010:
          m68k_stop(cpu, mem);
          break;

        case 0b011:
          m68k_rte(cpu, mem);
          break;

        case 0b101:
          m68k_rts(cpu, mem);
          break;

        case 0b110:
          m68k_trapv(cpu, mem);
          break;

        case 0b111:
          m68k_rtr(cpu, mem);
          break;

        default:
          panic("oneofthos");
          m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
          break;
        }
        break;

      default:
        m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
        break;
      }
      break;

    default:
      m68k_exception(cpu, mem, M68K_VECTOR_ILLEGAL_INSTRUCTION);
      break;
    }
    break;

  case 0b0101: /* ADDQ/SUBQ/Scc/DBcc/TRAPcc */
    switch ((opcode >> 6) & 0x3) {
    case 0b00:
    case 0b01:
    case 0b10:
      if ((opcode >> 8) & 1) {
        m68k_subq(cpu, mem, opcode);
      } else {
        m68k_addq(cpu, mem, opcode);
      }
      break;

    case 0b11:
      if (((opcode >> 3) & 0x7) == EA_MODE_AR_DIRECT) {
        m68k_dbcc(cpu, mem, opcode);
      } else {
        m68k_scc(cpu, mem, opcode);
      }
      break;
    }
    break;

  case 0b0110: /* Bcc/BSR/BRA */
    m68k_branch(cpu, mem, opcode);
    break;

  case 0b0111: /* MOVEQ */
    m68k_moveq(cpu, opcode);
    break;

  case 0b1000: /* OR/DIV/SBCD */
    m68k_or(cpu, mem, opcode);
    break;

  case 0b1001: /* SUB/SUBX */
    m68k_sub(cpu, mem, opcode);
    break;

  case 0b1010: /* (Unassigned, Reserved) */
    m68k_exception(cpu, mem, M68K_VECTOR_UNIMPLEMENTED_A_LINE_OPCODE);
    break;

  case 0b1011: /* CMP/EOR */
    m68k_cmp_eor(cpu, mem, opcode);
    break;

  case 0b1100: /* AND/MUL/ABCD/EXG */
    m68k_and(cpu, mem, opcode);
    break;

  case 0b1101: /* ADD/ADDX */
    m68k_add(cpu, mem, opcode);
    break;

  case 0b1110: /* Shift/Rotate/Bit Field */
    switch ((opcode >> 6) & 0x3) {
    case 0b00: /* Register, Byte */
      switch ((opcode >> 3) & 0x3) {
      case 0b00: /* Arithmetic Shift */
        m68k_as_reg_byte(cpu, opcode);
        break;

      case 0b01: /* Logical Shift */
        m68k_ls_reg_byte(cpu, opcode);
        break;

      case 0b10: /* Rotate with Extend */
        m68k_rox_reg_byte(cpu, opcode);
        break;

      case 0b11: /* Rotate */
        m68k_ro_reg_byte(cpu, opcode);
        break;
      }
      break;

    case 0b01: /* Register, Word */
      switch ((opcode >> 3) & 0x3) {
      case 0b00: /* Arithmetic Shift */
        m68k_as_reg_word(cpu, opcode);
        break;

      case 0b01: /* Logical Shift */
        m68k_ls_reg_word(cpu, opcode);
        break;

      case 0b10: /* Rotate with Extend */
        m68k_rox_reg_word(cpu, opcode);
        break;

      case 0b11: /* Rotate */
        m68k_ro_reg_word(cpu, opcode);
        break;
      }
      break;

    case 0b10: /* Register, Long */
      switch ((opcode >> 3) & 0x3) {
      case 0b00: /* Arithmetic Shift */
        m68k_as_reg_long(cpu, opcode);
        break;

      case 0b01: /* Logical Shift */
        m68k_ls_reg_long(cpu, opcode);
        break;

      case 0b10: /* Rotate with Extend */
        m68k_rox_reg_long(cpu, opcode);
        break;

      case 0b11: /* Rotate */
        m68k_ro_reg_long(cpu, opcode);
        break;
      }
      break;

    case 0b11: /* Memory */
      switch ((opcode >> 9) & 0x3) {
      case 0b00: /* Arithmetic Shift */
        m68k_as_mem(cpu, mem, opcode);
        break;

      case 0b01: /* Logical Shift */
        m68k_ls_mem(cpu, mem, opcode);
        break;

      case 0b10: /* Rotate with Extend */
        m68k_rox_mem(cpu, mem, opcode);
        break;

      case 0b11: /* Rotate */
        m68k_ro_mem(cpu, mem, opcode);
        break;
      }
      break;
    }
    break;

  case 0b1111: /* Coprocessor Interface/MC68040 and CPU32 Extensions */
    m68k_exception(cpu, mem, M68K_VECTOR_UNIMPLEMENTED_F_LINE_OPCODE);
    break;
  }

  m68k_trace_end();
}



void m68k_init(m68k_t *cpu)
{
  memset(cpu, 0, sizeof(m68k_t));
  cpu->status.s = 1; /* Always start in supervisor mode. */
}



