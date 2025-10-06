#ifndef _RAMDISK_H
#define _RAMDISK_H

#include <limits.h>
#include <stdint.h>
#include "mem.h"

#define RAMDISK_MAX 4
#define RAMDISK_TRACKS 512
#define RAMDISK_SECTORS 256 /* Per Track */
#define RAMDISK_SECTOR_SIZE 128
#define RAMDISK_SIZE (RAMDISK_TRACKS * RAMDISK_SECTORS * RAMDISK_SECTOR_SIZE)

typedef struct ramdisk_s {
  char filename[RAMDISK_MAX][PATH_MAX];
  uint8_t data[RAMDISK_MAX][RAMDISK_SIZE];
  uint8_t disk_no;
  uint16_t track_no;
  uint16_t sector_no;
  uint32_t dma_address;
} ramdisk_t;

uint32_t ramdisk_select(ramdisk_t *ramdisk, uint8_t value);
void ramdisk_track_set(ramdisk_t *ramdisk, uint16_t value);
void ramdisk_sector_set(ramdisk_t *ramdisk, uint16_t value);
void ramdisk_dma_set(ramdisk_t *ramdisk, uint32_t value);
void ramdisk_read(ramdisk_t *ramdisk, mem_t *mem);
void ramdisk_write(ramdisk_t *ramdisk, mem_t *mem);
void ramdisk_init(ramdisk_t *ramdisk);
int ramdisk_load(ramdisk_t *ramdisk, uint8_t disk_no, const char *filename);
int ramdisk_save(ramdisk_t *ramdisk, uint8_t disk_no, const char *filename);

#endif /* _RAMDISK_H */
