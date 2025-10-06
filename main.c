#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "console.h"
#include "debugger.h"
#include "m68k.h"
#include "m68k_trace.h"
#include "mem.h"
#include "panic.h"
#include "ramdisk.h"



#define CPM_BIOS_DEFAULT_FILENAME "emubios.srec"
#define CPM_BIOS_DEFAULT_ENTRY_POINT 0xFF0000

static m68k_t cpu;
static mem_t mem;
static ramdisk_t ramdisk;

static bool debugger_break = false;
static char panic_msg[80];



void panic(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vsnprintf(panic_msg, sizeof(panic_msg), format, args);
  va_end(args);

  debugger_break = true;
}



static void sig_handler(int sig)
{
  switch (sig) {
  case SIGINT:
    debugger_break = true;
    return;
  }
}



static void trap_hook(uint32_t d[8])
{
  static char filename[16];
  static char lc_filename[16];
  static FILE *fh;
  int c;
  int i;
  int n;

  switch (d[0]) {
  case 1: /* Console Status */
    d[0] = console_status();
    break;

  case 2: /* Console Read */
    d[0] = console_read();
    break;

  case 3: /* Console Write */
    console_write(d[1]);
    break;

  case 4: /* RAM Disk Select */
    d[0] = ramdisk_select(&ramdisk, d[1]);
    break;

  case 5: /* RAM Disk Track Set */
    ramdisk_track_set(&ramdisk, d[1]);
    break;

  case 6: /* RAM Disk Sector Set */
    ramdisk_sector_set(&ramdisk, d[1]);
    break;

  case 7: /* RAM Disk DMA Set */
    ramdisk_dma_set(&ramdisk, d[1]);
    break;

  case 8: /* RAM Disk Read */
    ramdisk_read(&ramdisk, &mem);
    break;

  case 9: /* RAM Disk Write */
    ramdisk_write(&ramdisk, &mem);
    break;

  case 10: /* Remote Open */
    memset(filename, '\0', sizeof(filename));
    memset(lc_filename, '\0', sizeof(lc_filename));
    n = 0;

    for (i = 0; i < 8; i++) {
      c = mem_read_byte(&mem, d[1] + i);
      if (c == 0x20) {
        break;
      }
      filename[n] = c;
      lc_filename[n] = tolower(c);
      n++;
    }

    for (i = 0; i < 3; i++) {
      c = mem_read_byte(&mem, d[1] + 8 + i);
      if (c == 0x20) {
        break;
      }
      if (i == 0) { /* Add dot if there is an extension. */
        filename[n] = '.';
        lc_filename[n] = '.';
        n++;
      }
      filename[n] = c;
      lc_filename[n] = tolower(c);
      n++;
    }

    fh = NULL;
    if (d[2] == 'w') {
      fh = fopen(filename, "wb");
    } else if (d[2] == 'r') {
      fh = fopen(filename, "rb");
      if (fh == NULL && errno == ENOENT) {
        fh = fopen(lc_filename, "rb"); /* Fallback to lowercase. */
      }
    }

    if (fh == NULL) {
      d[0] = 0xFF; /* Error */
    } else {
      d[0] = 0x00; /* OK */
    }
    break;

  case 11: /* Remote Write */
    if (fh == NULL) {
      d[0] = 0xFF; /* Error */
    } else {
      for (i = 0; i < 128; i++) {
        fputc(mem_read_byte(&mem, d[1] + i), fh);
      }
      d[0] = 0x00; /* OK */
    }
    break;

  case 12: /* Remote Read */
    if (fh == NULL) {
      d[0] = 0xFF; /* Error */
    } else {
      d[0] = 0x00; /* Maybe More */
      for (i = 0; i < 128; i++) {
        c = fgetc(fh);
        if (c == EOF) {
          if (i == 0) {
            d[0] = 0x01; /* Done */
            break;
          }
          c = '\0';
        }
        mem_write_byte(&mem, d[1] + i, c);
      }
    }
    break;

  case 13: /* Remote Close */
    if (fh != NULL) {
      fclose(fh);
    }
    fh = NULL;
    break;

  case 14: /* Quit */
    exit(EXIT_SUCCESS);
    break;

  default:
    break;
  }
}



static void display_help(const char *progname)
{
  fprintf(stdout, "Usage: %s <options> [ramdisk-image]\n", progname);
  fprintf(stdout, "Options:\n"
    "  -h        Display this help.\n"
    "  -d        Enter debugger on start.\n"
    "  -w        Enable warp mode to maximize host CPU usage.\n"
    "  -b FILE   Use S-record FILE as CP/M and BIOS instead of the default.\n"
    "  -e ADDR   Entry point at (hex) ADDR instead of the default.\n"
    "  -i STR    Inject STR as input (CP/M commands) to console.\n"
    "  -I FILE   Inject text from FILE as input (CP/M commands) to console.\n"
#if RAMDISK_MAX > 1
    "  -B FILE   Load FILE into RAM disk B.\n"
#endif
#if RAMDISK_MAX > 2
    "  -C FILE   Load FILE into RAM disk C.\n"
#endif
#if RAMDISK_MAX > 3
    "  -D FILE   Load FILE into RAM disk D.\n"
#endif
    "\n");
  fprintf(stdout,
    "Default CP/M and BIOS: '%s' @ 0x%06x\n",
      CPM_BIOS_DEFAULT_FILENAME, CPM_BIOS_DEFAULT_ENTRY_POINT);
  fprintf(stdout,
    "RAM disk image should be in binary format "
    "and will be loaded into disk A.\n");
  fprintf(stdout,
    "Using Ctrl+C will break into debugger, use 'q' from there to quit.\n\n");
}



int main(int argc, char *argv[])
{
  int c;
  int i;
  char *ramdisk_filename[RAMDISK_MAX];
  char *inject_string = NULL;
  char *inject_filename = NULL;
  char *cpm_bios_filename = CPM_BIOS_DEFAULT_FILENAME;
  uint32_t cpm_bios_entry_point = CPM_BIOS_DEFAULT_ENTRY_POINT;

  for (i = 0; i < RAMDISK_MAX; i++) {
    ramdisk_filename[i] = NULL;
  }

  panic_msg[0] = '\0';
  signal(SIGINT, sig_handler);

  while ((c = getopt(argc, argv, "hdwb:e:i:I:B:C:D:")) != -1) {
    switch (c) {
    case 'h':
      display_help(argv[0]);
      return EXIT_SUCCESS;

    case 'd':
      debugger_break = true;
      break;

    case 'w':
      console_warp_mode_toggle();
      break;

    case 'b':
      cpm_bios_filename = optarg;
      break;

    case 'e':
      sscanf(optarg, "%x", &cpm_bios_entry_point);
      break;

    case 'i':
      inject_string = optarg;
      break;

    case 'I':
      inject_filename = optarg;
      break;

#if RAMDISK_MAX > 1
    case 'B':
      ramdisk_filename[1] = optarg;
      break;
#endif

#if RAMDISK_MAX > 2
    case 'C':
      ramdisk_filename[2] = optarg;
      break;
#endif

#if RAMDISK_MAX > 3
    case 'D':
      ramdisk_filename[3] = optarg;
      break;
#endif

    case '?':
    default:
      display_help(argv[0]);
      return EXIT_FAILURE;
    }
  }

  if (argc > optind) {
    ramdisk_filename[0] = argv[optind]; /* Disk A */
  }

  m68k_trace_init();
  console_init();
  mem_init(&mem);
  ramdisk_init(&ramdisk);
  m68k_init(&cpu);
  cpu.trap_15_hook = trap_hook;

  for (i = 0; i < RAMDISK_MAX; i++) {
    if (ramdisk_filename[i] != NULL) {
      if (ramdisk_load(&ramdisk, i, ramdisk_filename[i]) != 0) {
        fprintf(stdout, "Loading RAM disk %c file '%s' failed!\n",
          i + 0x41, ramdisk_filename[i]);
        return EXIT_FAILURE;
      }
    }
  }

  if (mem_load_srec(&mem, cpm_bios_filename) != 0) {
    fprintf(stdout, "Loading CP/M and BIOS file '%s' failed!\n",
      cpm_bios_filename);
    return EXIT_FAILURE;
  }

  if (inject_filename != NULL) {
    if (console_inject_file(inject_filename) != 0) {
      fprintf(stdout, "Injecting file '%s' failed!\n", inject_filename);
      return EXIT_FAILURE;
    }
  }

  if (inject_string != NULL) {
    while (*inject_string != '\0') {
      console_inject(*inject_string);
      inject_string++;
    }
  }

  cpu.pc = cpm_bios_entry_point;

  while (1) {
    if (debugger_break) {
      console_pause();
      if (panic_msg[0] != '\0') {
        fprintf(stdout, "%s", panic_msg);
        panic_msg[0] = '\0';
      }
      debugger_break = debugger(&cpu, &mem, &ramdisk);
      if (! debugger_break) {
        console_resume();
      }
    }

    m68k_execute(&cpu, &mem);

#ifdef CPU_BREAKPOINT
    if ((int32_t)cpu.pc == debugger_breakpoint_pc) {
      panic("Breakpoint\n");
    }
#endif /* CPU_BREAKPOINT */
  }

  return EXIT_SUCCESS;
}



