#define _POSIX_C_SOURCE 200809L
#include "utils.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "server.h"

int daemon_mode = 0;

void log_msg(int priority, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (daemon_mode)
    vsyslog(priority, fmt, ap);
  else {
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
  }
  va_end(ap);
}

void close_fd(int fd, const char* label) {
  if (close(fd) < 0) log_msg(LOG_ERR, "Error closing %s: %s", label, strerror(errno));
}

ssize_t safe_dprintf(int fd, const char* format, ...) {
  va_list args;
  va_start(args, format);
  ssize_t ret = vdprintf(fd, format, args);
  va_end(args);

  if (ret < 0) {
    perror("dprintf error: ");
  }
  return ret;
}
