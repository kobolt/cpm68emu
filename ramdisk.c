#include "ramdisk.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "mem.h"
#include "panic.h"



uint32_t ramdisk_select(ramdisk_t *ramdisk, uint8_t value)
{
  if (value >= RAMDISK_MAX) {
    return 0x0; /* Disk does not exist. */
  } else {
    ramdisk->disk_no = value;
    return 0xFFFFFFFF; /* OK, let BIOS set DPH value. */
  }
}



void ramdisk_track_set(ramdisk_t *ramdisk, uint16_t value)
{
  if (value >= RAMDISK_TRACKS) {
    panic("RAM disk track out of bounds: %d\n", value);
  } else {
    ramdisk->track_no = value;
  }
}



void ramdisk_sector_set(ramdisk_t *ramdisk, uint16_t value)
{
  if (value >= RAMDISK_SECTORS) {
    panic("RAM disk sector out of bounds: %d\n", value);
  } else {
    ramdisk->sector_no = value;
  }
}



void ramdisk_dma_set(ramdisk_t *ramdisk, uint32_t value)
{
  ramdisk->dma_address = value;
}



void ramdisk_read(ramdisk_t *ramdisk, mem_t *mem)
{
  int i;
  uint32_t offset;

  offset = ((ramdisk->track_no * RAMDISK_SECTORS)
    + ramdisk->sector_no)
    * RAMDISK_SECTOR_SIZE;
  for (i = 0; i < RAMDISK_SECTOR_SIZE; i++) {
    mem_write_byte(mem, ramdisk->dma_address + i,
      ramdisk->data[ramdisk->disk_no][offset + i]);
  }
}



void ramdisk_write(ramdisk_t *ramdisk, mem_t *mem)
{
  int i;
  uint32_t offset;

  offset = ((ramdisk->track_no * RAMDISK_SECTORS)
    + ramdisk->sector_no)
    * RAMDISK_SECTOR_SIZE;
  for (i = 0; i < RAMDISK_SECTOR_SIZE; i++) {
    ramdisk->data[ramdisk->disk_no][offset + i] =
      mem_read_byte(mem, ramdisk->dma_address + i);
  }
}



void ramdisk_init(ramdisk_t *ramdisk)
{
  int i;
  size_t n;

  ramdisk->disk_no = 0;
  ramdisk->track_no = 0;
  ramdisk->sector_no = 0;
  ramdisk->dma_address = 0;

  for (i = 0; i < RAMDISK_MAX; i++) {
    for (n = 0; n < RAMDISK_SIZE; n++) {
      ramdisk->data[i][n] = 0xE5; /* Fill space to indicate no files. */
    }
    ramdisk->filename[i][0] = '\0';
  }
}



int ramdisk_load(ramdisk_t *ramdisk, uint8_t disk_no, const char *filename)
{
  FILE *fh;
  size_t n;
  int c;

  if (disk_no > RAMDISK_MAX) {
    return -2;
  }

  if (filename == NULL) {
    return -3;
  }

  fh = fopen(filename, "rb");
  if (fh == NULL) {
    return -1;
  }

  for (n = 0; n < RAMDISK_SIZE; n++) {
    c = fgetc(fh);
    if (c == EOF) {
      break;
    }
    ramdisk->data[disk_no][n] = c;
  }

  fclose(fh);
  strncpy(ramdisk->filename[disk_no], filename, PATH_MAX);
  return 0;
}



int ramdisk_save(ramdisk_t *ramdisk, uint8_t disk_no, const char *filename)
{
  FILE *fh;
  size_t n;

  if (disk_no > RAMDISK_MAX) {
    return -2;
  }

  if (filename == NULL) {
    if (ramdisk->filename[disk_no][0] == '\0') {
      return -3;
    }
    fh = fopen(ramdisk->filename[disk_no], "wb");
  } else {
    fh = fopen(filename, "wb");
  }

  if (fh == NULL) {
    return -1;
  }

  for (n = 0; n < RAMDISK_SIZE; n++) {
    fputc(ramdisk->data[disk_no][n], fh);
  }
  fclose(fh);

  if (filename != NULL) {
    strncpy(ramdisk->filename[disk_no], filename, PATH_MAX);
  }

  return 0;
}



