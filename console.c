#include "console.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include "panic.h"



#define CONSOLE_INJECT_MAX 65536

/* NOTE: This pause hack is needed to prevent overflowing the CP/M key input
   buffer which causes unpredictable results. Adjust if needed. */
#define CONSOLE_INJECT_PAUSE 100

static uint8_t console_inject_buffer[CONSOLE_INJECT_MAX];
static uint32_t console_inject_head = 0;
static uint32_t console_inject_tail = 0;
static uint32_t console_inject_pause = 0;

static int console_poll_timeout = 1;



uint8_t console_status(void)
{
  int result;
  struct pollfd fds[1];

  if (console_inject_tail != console_inject_head) {
    if (console_inject_pause > 0) {
      console_inject_pause--;
      return 0x00; /* Wait (before getting more from inject buffer). */
    } else {
      console_inject_pause = CONSOLE_INJECT_PAUSE;
      return 0x01; /* Data available (from inject buffer). */
    }
  }

  fds[0].fd = STDIN_FILENO;
  fds[0].events = POLLIN;
  result = poll(fds, 1, console_poll_timeout); /* Relax host CPU if possible. */
  if (result > 0) {
    return 0x01; /* Data available. */
  } else if (result == -1) {
    if (errno != EINTR) {
      panic("poll() failed with errno: %d\n", errno);
    }
  }
  return 0x00; /* No data. */
}



uint8_t console_read(void)
{
  int c;

  if (console_inject_tail != console_inject_head) {
    c = console_inject_buffer[console_inject_tail];
    console_inject_tail++;
    if (console_inject_tail >= CONSOLE_INJECT_MAX) {
      console_inject_tail = 0;
    }
    return c;
  }

  c = fgetc(stdin);
  if (c == EOF) {
    exit(EXIT_SUCCESS);
  }

  if (c == 0x7F) {
    c = 0x08; /* Convert DEL to BS for backspace to work correctly. */
  }

  if (c == 0x0A) {
    c = 0x0D; /* Convert LF to CR for better compatibility. */
  }

  return c & 255;
}



void console_write(uint8_t value)
{
  fputc(value, stdout);
}



void console_inject(uint8_t value)
{
  console_inject_buffer[console_inject_head] = value;
  console_inject_head++;
  if (console_inject_head >= CONSOLE_INJECT_MAX) {
    console_inject_head = 0;
  }
}



int console_inject_file(const char *filename)
{
  FILE *fh;
  int c;

  if (filename == NULL) {
    return -2;
  }

  fh = fopen(filename, "r");
  if (fh == NULL) {
    return -1;
  }

  while ((c = fgetc(fh)) != EOF) {
    console_inject(c);
  }

  fclose(fh);
  return 0;
}



bool console_warp_mode_toggle(void)
{
  if (console_poll_timeout == 0) {
    console_poll_timeout = 1;
    return false; /* Warp mode disabled. */
  } else {
    console_poll_timeout = 0;
    return true; /* Warp mode enabled. */
  }
}



void console_pause(void)
{
  struct termios ts;
  int flags;

  /* Restore canonical mode and echo. */
  tcgetattr(STDIN_FILENO, &ts);
  ts.c_lflag |= ICANON | ECHO;
  tcsetattr(STDIN_FILENO, TCSANOW, &ts);

  /* Disable non-blocking mode. */
  flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (flags != -1) {
    (void)fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
  }
}



void console_resume(void)
{
  struct termios ts;
  int flags;

  /* Turn off canonical mode and echo. */
  tcgetattr(STDIN_FILENO, &ts);
  ts.c_lflag &= ~ICANON & ~ECHO;
  tcsetattr(STDIN_FILENO, TCSANOW, &ts);

  /* Enable non-blocking mode. */
  flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (flags != -1) {
    (void)fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
  }
}



void console_init(void)
{
  atexit(console_pause);
  console_resume();

  /* Make stdout unbuffered. */
  setvbuf(stdout, NULL, _IONBF, 0);
}



