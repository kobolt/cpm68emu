#include "debugger.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "console.h"
#include "m68k.h"
#include "m68k_trace.h"
#include "mem.h"
#include "panic.h"
#include "ramdisk.h"



#define DEBUGGER_ARGS 3

#ifdef CPU_BREAKPOINT
int32_t debugger_breakpoint_pc = -1;
#endif /* CPU_BREAKPOINT */



static void debugger_help(void)
{
  fprintf(stdout, "Commands:\n");
  fprintf(stdout, "  q              - Quit\n");
  fprintf(stdout, "  h              - Help\n");
  fprintf(stdout, "  c              - Continue\n");
  fprintf(stdout, "  s              - Step\n");
  fprintf(stdout, "  w              - Toggle Warp Mode\n");
  fprintf(stdout, "  z [key]        - Send Ctrl+<Key>\n");
#ifdef CPU_BREAKPOINT
  fprintf(stdout, "  b <addr>       - Breakpoint\n");
#endif /* CPU_BREAKPOINT */
  fprintf(stdout, "  t [full]       - Dump CPU Trace\n");
  fprintf(stdout, "  d <addr> [end] - Dump Memory\n");
  fprintf(stdout, "  f [filename]   - Save RAM Disk A\n");
}



static bool debugger_overwrite(FILE *out, FILE *in, const char *filename)
{
  struct stat st;
  char answer[2];

  if (stat(filename, &st) == 0) {
    if (S_ISREG(st.st_mode)) {
      while (1) {
        fprintf(out, "\rOverwrite '%s' (y/n) ? ", filename);
        if (fgets(answer, sizeof(answer), in) == NULL) {
          if (feof(stdin)) {
            return false;
          }
        } else {
          if (answer[0] == 'y') {
            return true;
          } else if (answer[0] == 'n') {
            return false;
          }
        }
      }

    } else {
      fprintf(out, "Filename is not a file!\n");
      return false;
    }

  } else {
    if (errno == ENOENT) {
      return true; /* File not found, OK to write. */

    } else {
      fprintf(out, "stat() failed with errno: %d\n", errno);
      return false;
    }
  }
}



bool debugger(m68k_t *cpu, mem_t *mem, ramdisk_t *ramdisk)
{
  char input[128];
  char *argv[3];
  int argc;
  int value1;
  int value2;
  int result;

  fprintf(stdout, "\n");
  while (1) {
    fprintf(stdout, "\r%06x> ", cpu->pc);

    if (fgets(input, sizeof(input), stdin) == NULL) {
      if (feof(stdin)) {
        exit(EXIT_SUCCESS);
      }
      continue;
    }

    if ((strlen(input) > 0) && (input[strlen(input) - 1] == '\n')) {
      input[strlen(input) - 1] = '\0'; /* Strip newline. */
    }

    argv[0] = strtok(input, " ");
    if (argv[0] == NULL) {
      continue;
    }

    for (argc = 1; argc < 3; argc++) {
      argv[argc] = strtok(NULL, " ");
      if (argv[argc] == NULL) {
        break;
      }
    }

    if (strncmp(argv[0], "q", 1) == 0) {
      exit(EXIT_SUCCESS);

    } else if (strncmp(argv[0], "?", 1) == 0) {
      debugger_help();

    } else if (strncmp(argv[0], "h", 1) == 0) {
      debugger_help();

    } else if (strncmp(argv[0], "c", 1) == 0) {
      return false;

    } else if (strncmp(argv[0], "s", 1) == 0) {
      return true;

    } else if (strncmp(argv[0], "w", 1) == 0) {
      if (console_warp_mode_toggle()) {
        fprintf(stdout, "Warp mode enabled.\n");
      } else {
        fprintf(stdout, "Warp mode disabled.\n");
      }

    } else if (strncmp(argv[0], "z", 1) == 0) {
      if (argc >= 2) {
        value1 = argv[1][0];
        if ((value1 >= 0x41) && (value1 <= 0x5A)) {
          console_inject(value1 - 0x40);
          fprintf(stdout, "Ctrl+%c sent.\n", value1);
        } else if ((value1 >= 0x61) && (value1 <= 0x7A)) {
          console_inject(value1 - 0x60);
          fprintf(stdout, "Ctrl+%c sent.\n", value1 - 0x20);
        } else {
          fprintf(stdout, "Invalid argument! (Use 'a' to 'z'.)\n");
        }
      } else {
        fprintf(stdout, "Missing argument!\n");
      }

#ifdef CPU_BREAKPOINT
    } else if (strncmp(argv[0], "b", 1) == 0) {
      if (argc >= 2) {
        if (sscanf(argv[1], "%6x", &value1) == 1) {
          debugger_breakpoint_pc = (value1 & 0xFFFFFF);
          fprintf(stdout, "Breakpoint at 0x%06x set.\n",
            debugger_breakpoint_pc);
        } else {
          fprintf(stdout, "Invalid argument!\n");
        }
      } else {
        if (debugger_breakpoint_pc < 0) {
          fprintf(stdout, "Missing argument!\n");
        } else {
          fprintf(stdout, "Breakpoint at 0x%06x removed.\n",
            debugger_breakpoint_pc);
        }
        debugger_breakpoint_pc = -1;
      }
#endif /* CPU_BREAKPOINT */

    } else if (strncmp(argv[0], "t", 1) == 0) {
      if (argc >= 2) {
        m68k_trace_dump(stdout, false);
      } else {
        m68k_trace_dump(stdout, true);
      }

    } else if (strncmp(argv[0], "d", 1) == 0) {
      if (argc >= 3) {
        sscanf(argv[1], "%6x", &value1);
        sscanf(argv[2], "%6x", &value2);
        mem_dump(stdout, mem, (uint32_t)value1, (uint32_t)value2);
      } else if (argc >= 2) {
        sscanf(argv[1], "%6x", &value1);
        value2 = value1 + 0xFF;
        if (value2 > 0xFFFFFF) {
          value2 = 0xFFFFFF; /* Truncate */
        }
        mem_dump(stdout, mem, (uint32_t)value1, (uint32_t)value2);
      } else {
        fprintf(stdout, "Missing argument!\n");
      }

    } else if (strncmp(argv[0], "f", 1) == 0) {
      if (argc >= 2) {
        if (debugger_overwrite(stdout, stdin, argv[1])) {
          result = ramdisk_save(ramdisk, 0, argv[1]);
          if (result == 0) {
            fprintf(stdout, "RAM disk A saved.\n");
          } else {
            fprintf(stdout, "RAM disk A save error: %d\n", result);
          }
        }
      } else {
        if (debugger_overwrite(stdout, stdin, ramdisk->filename[0])) {
          result = ramdisk_save(ramdisk, 0, NULL);
          if (result == 0) {
            fprintf(stdout, "RAM disk A saved.\n");
          } else {
            fprintf(stdout, "RAM disk A save error: %d\n", result);
          }
        }
      }

    } else {
      fprintf(stdout, "Unknown command: '%c' (use 'h' for help.)\n",
        argv[0][0]);
    }
  }
}



