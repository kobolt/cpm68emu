#include "mem.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>



uint8_t mem_read_byte(mem_t *mem, uint32_t address)
{
  return mem->ram[address & 0xFFFFFF];
}



uint16_t mem_read_word(mem_t *mem, uint32_t address, bool *error)
{
  address &= 0xFFFFFF;
  if (address % 2 != 0) {
    *error = true;
    return 0;
  } else {
    return mem->ram[address+1] |
          (mem->ram[address] << 8);
  }
}



uint32_t mem_read_long(mem_t *mem, uint32_t address, bool *error)
{
  address &= 0xFFFFFF;
  if (address % 2 != 0) {
    *error = true;
    return 0;
  } else {
    if (address == 0xFFFFFE) {
      return mem->ram[0x000001]         |
            (mem->ram[0x000000]  << 8)  |
            (mem->ram[address+1] << 16) |
            (mem->ram[address]   << 24);
    } else {
      return mem->ram[address+3]        |
            (mem->ram[address+2] << 8)  |
            (mem->ram[address+1] << 16) |
            (mem->ram[address]   << 24);
    }
  }
}



void mem_write_byte(mem_t *mem, uint32_t address, uint8_t value)
{
  mem->ram[address & 0xFFFFFF] = value;
}



void mem_write_word(mem_t *mem, uint32_t address, uint16_t value, bool *error)
{
  address &= 0xFFFFFF;
  if (address % 2 != 0) {
    *error = true;
  } else {
    mem->ram[address]   = (value >> 8) & 0xFF;
    mem->ram[address+1] =  value       & 0xFF;
  }
}



void mem_write_long(mem_t *mem, uint32_t address, uint32_t value, bool *error)
{
  address &= 0xFFFFFF;
  if (address % 2 != 0) {
    *error = true;
  } else {
    if (address == 0xFFFFFE) {
      mem->ram[address  ] = (value >> 24) & 0xFF;
      mem->ram[address+1] = (value >> 16) & 0xFF;
      mem->ram[0x000000]  = (value >> 8)  & 0xFF;
      mem->ram[0x000001]  =  value        & 0xFF;
    } else {
      mem->ram[address  ] = (value >> 24) & 0xFF;
      mem->ram[address+1] = (value >> 16) & 0xFF;
      mem->ram[address+2] = (value >> 8)  & 0xFF;
      mem->ram[address+3] =  value        & 0xFF;
    }
  }
}



void mem_init(mem_t *mem)
{
  int i;
  for (i = 0; i < MEM_MAX; i++) {
    mem->ram[i] = 0x0;
  }
}



int mem_load_binary(mem_t *mem, const char *filename, uint32_t address)
{
  FILE *fh;
  int c;

  fh = fopen(filename, "rb");
  if (fh == NULL) {
    return -1;
  }

  while ((c = fgetc(fh)) != EOF) {
    mem_write_byte(mem, address, c);
    address++;
  }

  fclose(fh);
  return 0;
}



int mem_load_srec(mem_t *mem, const char *filename)
{
  FILE *fh;
  int i;
  int n;
  char line[128];
  uint8_t count;
  uint8_t type;
  uint8_t byte;
  uint32_t address;

  fh = fopen(filename, "r");
  if (fh == NULL) {
    return -1;
  }

  while (fgets(line, sizeof(line), fh) != NULL) {
    if (line[0] != 'S') {
      continue; /* Line must start with 'S'. */
    }

    type = line[1];
    if (! (type == '1' || type == '2')) {
      continue; /* Only 16-bit or 24-bit load address supported. */
    }

    if (sscanf(&line[2], "%02hhX", &count) != 1) {
      continue; /* Unable to read byte count. */
    }

    if (type == '1') {
      if (sscanf(&line[4], "%04X", &address) != 1) {
        continue; /* Unable to read address. */
      }
      i = 8;
    } else if (type == '2') {
      if (sscanf(&line[4], "%06X", &address) != 1) {
        continue; /* Unable to read address. */
      }
      i = 10;
    }

    /* Read data bytes: */
    for (n = 0; i < (count * 2) + 2; i += 2, n++) {
      if (sscanf(&line[i], "%02hhX", &byte) != 1) {
        continue; /* Unable to read byte. */
      }
      mem_write_byte(mem, address + n, byte);
    }
  }

  fclose(fh);
  return 0;
}



static void mem_dump_16(FILE *fh, mem_t *mem, uint32_t start, uint32_t end)
{
  int i;
  uint32_t address;
  uint8_t value;

  fprintf(fh, "%06x   ", start & 0xFFFFF0);

  /* Hex */
  for (i = 0; i < 16; i++) {
    address = (start & 0xFFFFF0) + i;
    value = mem_read_byte(mem, address);
    if ((address >= start) && (address <= end)) {
      fprintf(fh, "%02x ", value);
    } else {
      fprintf(fh, "   ");
    }
    if (i % 4 == 3) {
      fprintf(fh, " ");
    }
  }

  /* Character */
  for (i = 0; i < 16; i++) {
    address = (start & 0xFFFFF0) + i;
    value = mem_read_byte(mem, address);
    if ((address >= start) && (address <= end)) {
      if (isprint(value)) {
        fprintf(fh, "%c", value);
      } else {
        fprintf(fh, ".");
      }
    } else {
      fprintf(fh, " ");
    }
  }

  fprintf(fh, "\n");
}



void mem_dump(FILE *fh, mem_t *mem, uint32_t start, uint32_t end)
{
  uint32_t i;
  mem_dump_16(fh, mem, start, end);
  for (i = (start & 0xFFFFF0) + 16; i <= end; i += 16) {
    mem_dump_16(fh, mem, i, end);
  }
}



