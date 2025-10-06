#include "m68k_trace.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m68k.h"



#define M68K_TRACE_BUFFER_SIZE 64
#define M68K_TRACE_MC_MAX 8
#define M68K_TRACE_OP_MNEMONIC_MAX 16
#define M68K_TRACE_OP_SRC_MAX 16
#define M68K_TRACE_OP_DST_MAX 16

typedef struct m68k_trace_s {
  m68k_t cpu;
  uint16_t mc[M68K_TRACE_MC_MAX];
  int mc_n;
  char op_mnemonic[M68K_TRACE_OP_MNEMONIC_MAX];
  char op_src[M68K_TRACE_OP_SRC_MAX];
  char op_dst[M68K_TRACE_OP_DST_MAX];
} m68k_trace_t;



static m68k_trace_t trace_buffer[M68K_TRACE_BUFFER_SIZE];
static int trace_buffer_n = 0;



void m68k_trace_start(m68k_t *cpu)
{
  memcpy(&trace_buffer[trace_buffer_n].cpu, cpu, sizeof(m68k_t));
  trace_buffer[trace_buffer_n].mc_n = 0; /* Clear before use! */
  trace_buffer[trace_buffer_n].op_mnemonic[0] = '\0';
  trace_buffer[trace_buffer_n].op_src[0] = '\0';
  trace_buffer[trace_buffer_n].op_dst[0] = '\0';
}



void m68k_trace_mc(uint16_t mc)
{
  trace_buffer[trace_buffer_n].mc[trace_buffer[trace_buffer_n].mc_n] = mc;
  trace_buffer[trace_buffer_n].mc_n++;
  if (trace_buffer[trace_buffer_n].mc_n >= M68K_TRACE_MC_MAX) {
    trace_buffer[trace_buffer_n].mc_n = 0;
  }
}



void m68k_trace_op_mnemonic(const char *s)
{
  strncpy(trace_buffer[trace_buffer_n].op_mnemonic, s,
    M68K_TRACE_OP_MNEMONIC_MAX);
}



void m68k_trace_op_src(const char *format, ...)
{
  va_list args;
  va_start(args, format);
  vsnprintf(trace_buffer[trace_buffer_n].op_src,
    M68K_TRACE_OP_SRC_MAX, format, args);
  va_end(args);
}



void m68k_trace_op_dst(const char *format, ...)
{
  va_list args;
  va_start(args, format);
  vsnprintf(trace_buffer[trace_buffer_n].op_dst,
    M68K_TRACE_OP_DST_MAX, format, args);
  va_end(args);
}



void m68k_trace_end(void)
{
  trace_buffer_n++;
  if (trace_buffer_n >= M68K_TRACE_BUFFER_SIZE) {
    trace_buffer_n = 0;
  }
}



void m68k_trace_init(void)
{
  int i;

  for (i = 0; i < M68K_TRACE_BUFFER_SIZE; i++) {
    memset(&trace_buffer[i], 0, sizeof(m68k_trace_t));
  }
  trace_buffer_n = 0;
}



static void m68k_trace_print(FILE *fh, m68k_trace_t *trace, bool compact)
{
  int i;

  if (compact) {
    fprintf(fh, "%06x   ", trace->cpu.pc);

    for (i = 0; i < trace->mc_n; i++) {
      fprintf(fh, "%04x ", trace->mc[i]);
    }
    for (; i < 6; i++) {
      fprintf(fh, "     ");
    }

  } else {
    fprintf(fh, "D0-7 %08x %08x %08x %08x %08x %08x %08x %08x\n",
      trace->cpu.d[0],
      trace->cpu.d[1],
      trace->cpu.d[2],
      trace->cpu.d[3],
      trace->cpu.d[4],
      trace->cpu.d[5],
      trace->cpu.d[6],
      trace->cpu.d[7]);

    fprintf(fh, "A0-7 %08x %08x %08x %08x %08x %08x %08x %08x\n",
      trace->cpu.a[0],
      trace->cpu.a[1],
      trace->cpu.a[2],
      trace->cpu.a[3],
      trace->cpu.a[4],
      trace->cpu.a[5],
      trace->cpu.a[6],
      trace->cpu.a[7]);

    fprintf(fh, "  PC %08x       SR 10SM-210---XNZVC       SSP %08x\n",
      trace->cpu.pc, trace->cpu.ssp);

    fprintf(fh, "                       %d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d\n",
      trace->cpu.status.t1,
      trace->cpu.status.t0,
      trace->cpu.status.s,
      trace->cpu.status.m,
      trace->cpu.status.u4,
      trace->cpu.status.i2,
      trace->cpu.status.i1,
      trace->cpu.status.i0,
      trace->cpu.status.u3,
      trace->cpu.status.u2,
      trace->cpu.status.u1,
      trace->cpu.status.x,
      trace->cpu.status.n,
      trace->cpu.status.z,
      trace->cpu.status.v,
      trace->cpu.status.c);

    for (i = 0; i < trace->mc_n; i++) {
      fprintf(fh, "%04x ", trace->mc[i]);
    }
    for (; i < M68K_TRACE_MC_MAX + 1; i++) {
      fprintf(fh, "     ");
    }
  }

  if (trace->op_dst[0] == '\0' && trace->op_src[0] == '\0') {
    fprintf(fh, "%s\n", trace->op_mnemonic);
  } else if (trace->op_src[0] == '\0') {
    fprintf(fh, "%s %s\n", trace->op_mnemonic, trace->op_dst);
  } else if (trace->op_dst[0] == '\0') {
    fprintf(fh, "%s %s\n", trace->op_mnemonic, trace->op_src);
  } else {
    fprintf(fh, "%s %s, %s\n",
      trace->op_mnemonic, trace->op_src, trace->op_dst);
  }
}



void m68k_trace_dump(FILE *fh, bool compact)
{
  int i;
  for (i = trace_buffer_n; i < M68K_TRACE_BUFFER_SIZE; i++) {
    if (trace_buffer[i].mc_n != 0 && trace_buffer[i].op_mnemonic[0] != '\0') {
      m68k_trace_print(fh, &trace_buffer[i], compact);
    }
  }
  for (i = 0; i < trace_buffer_n; i++) {
    if (trace_buffer[i].mc_n != 0 && trace_buffer[i].op_mnemonic[0] != '\0') {
      m68k_trace_print(fh, &trace_buffer[i], compact);
    }
  }
}



