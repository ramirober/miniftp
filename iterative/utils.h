#pragma once

#include <sys/types.h>
#include <syslog.h>

// Set to 1 after daemon() is called; routes log_msg to syslog instead of stderr.
// Se setea a 1 después de llamar a daemon(), para que log_msg envíe mensajes a syslog en lugar de
// stderr

// Es extern para que pueda ser accedido desde cualquier archivo, no solo desde utils.c (una suerte
// de variable global)
extern int daemon_mode;

void close_fd(int fd, const char* label);
ssize_t safe_dprintf(int fd, const char* format, ...);
void log_msg(int priority, const char* fmt, ...);
