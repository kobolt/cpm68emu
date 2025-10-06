#ifndef _MEM_H
#define _MEM_H

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#define MEM_MAX 0x1000000 /* 24-bit */

typedef struct mem_s {
  uint8_t ram[MEM_MAX];
} mem_t;

uint8_t mem_read_byte(mem_t *mem, uint32_t address);
uint16_t mem_read_word(mem_t *mem, uint32_t address, bool *error);
uint32_t mem_read_long(mem_t *mem, uint32_t address, bool *error);
void mem_write_byte(mem_t *mem, uint32_t address, uint8_t value);
void mem_write_word(mem_t *mem, uint32_t address, uint16_t value, bool *error);
void mem_write_long(mem_t *mem, uint32_t address, uint32_t value, bool *error);

void mem_init(mem_t *mem);
int mem_load_binary(mem_t *mem, const char *filename, uint32_t address);
int mem_load_srec(mem_t *mem, const char *filename);
void mem_dump(FILE *fh, mem_t *mem, uint32_t start, uint32_t end);

#endif /* _MEM_H */
