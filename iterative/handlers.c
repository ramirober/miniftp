// handlers.c

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "dtp.h"
#include "pi.h"
#include "responses.h"
#include "session.h"
#include "utils.h"

void handle_USER(const char* args) {
  ftp_session_t* sess = session_get();

  if (!args || strlen(args) == 0) {
    safe_dprintf(sess->control_sock, MSG_501);  // Syntax error in parameters
    return;
  }

  strncpy(sess->current_user, args, sizeof(sess->current_user) - 1);
  sess->current_user[sizeof(sess->current_user) - 1] = '\0';
  safe_dprintf(sess->control_sock, MSG_331);  // Username okay, need password
}

void handle_PASS(const char* args) {
  ftp_session_t* sess = session_get();

  if (sess->current_user[0] == '\0') {
    safe_dprintf(sess->control_sock, MSG_503);  // Bad sequence of commands
    return;
  }

  if (!args || strlen(args) == 0) {
    safe_dprintf(sess->control_sock, MSG_501);  // Syntax error in parameters
    return;
  }

  if (check_credentials(sess->current_user, (char*)args) == 0) {
    sess->logged_in = 1;
    safe_dprintf(sess->control_sock, MSG_230);  // User logged in
  } else {
    safe_dprintf(sess->control_sock, MSG_530);  // Not logged in
    sess->current_user[0] = '\0';               // Reset user on failed login
    sess->logged_in = 0;
  }
}

void handle_QUIT(const char* args) {
  ftp_session_t* sess = session_get();
  (void)args;  // unused

  safe_dprintf(sess->control_sock, MSG_221);      // 221 Goodbye.
  sess->current_user[0] = '\0';                   // Close session
  close_fd(sess->control_sock, "client socket");  // Close socket
  sess->control_sock = -1;
}

void handle_SYST(const char* args) {
  ftp_session_t* sess = session_get();
  (void)args;  // unused

  safe_dprintf(sess->control_sock, MSG_215);  // 215 <system type>
}

void handle_TYPE(const char* args) {
  ftp_session_t* sess = session_get();
  (void)args;
  (void)sess;

  // Placeholder
}

// handle_PORT se encarga de establecer el puerto de datos para la conexion
void handle_PORT(const char* args) {
  ftp_session_t* sess = session_get();

  if (!sess->logged_in) {
    safe_dprintf(sess->control_sock, MSG_530);
    return;
  }

  if (!args || *args == '\0') {
    safe_dprintf(sess->control_sock, MSG_501);
    return;
  }

  // Formato: "h1,h2,h3,h4,p1,p2" (decimal, 0-255 cada campo).
  // IP = h1.h2.h3.h4, puerto = p1*256 + p2.
  unsigned int h[4], p[2];
  char tail;  // detecta basura después de los 6 enteros
  int n = sscanf(args, "%u,%u,%u,%u,%u,%u%c", &h[0], &h[1], &h[2], &h[3], &p[0], &p[1], &tail);

  // Usamos la constante MSG_501 para indicar que el usuario ha enviado un argumento invalido
  if (n != 6) {
    safe_dprintf(sess->control_sock, MSG_501);
    return;
  }
  for (int i = 0; i < 4; i++) {
    if (h[i] > 255) {
      safe_dprintf(sess->control_sock, MSG_501);
      return;
    }
  }
  if (p[0] > 255 || p[1] > 255) {
    safe_dprintf(sess->control_sock, MSG_501);
    return;
  }

  uint16_t port = (uint16_t)((p[0] << 8) | p[1]);
  if (port == 0) {
    // 0 lo usamos como "PORT no fijado"; ademas es invalido
    safe_dprintf(sess->control_sock, MSG_501);
    return;
  }

  // Si hay un canal de datos abierto por un PORT previo, cerrarlo.
  if (sess->data_sock >= 0) {
    close_fd(sess->data_sock, "previous data socket");
    sess->data_sock = -1;
  }

  // Nuevamente con memset creamos un buffer de ceros, asignamos el puerto a 0 para que el kernel
  // elija uno como fallback
  memset(&sess->data_addr, 0, sizeof(sess->data_addr));
  sess->data_addr.sin_family = AF_INET;
  sess->data_addr.sin_port = htons(port);

  // Este calculo devuelve un entero con la direccion IP en formato decimal, y con htonl convierte
  // el entero a formato long
  sess->data_addr.sin_addr.s_addr = htonl(((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
                                          ((uint32_t)h[2] << 8) | (uint32_t)h[3]);

  char ip_buf[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &sess->data_addr.sin_addr, ip_buf, sizeof(ip_buf));
  log_msg(LOG_INFO, "PORT guardado: %s:%u", ip_buf, port);

  safe_dprintf(sess->control_sock, MSG_200);
}

// handle_RETR se encarga de transferir el fichero solicitado por el cliente
// a traves del canal de datos previamente configurado con PORT
// 1) Validaciones previas (login, args, archivo existe) -> 530/501/550
// 2) 150 anunciando que abrimos el canal de datos
// 3) abrir canal -> si falla -> 425
// 4) transferir bytes -> si falla a mitad -> 451
// 5) cerrar canal y 226
void handle_RETR(const char* args) {
  ftp_session_t* sess = session_get();
  int fd;

  if (!sess->logged_in) {
    safe_dprintf(sess->control_sock, MSG_530);
    return;
  }

  if (!args || *args == '\0') {  // No hay argumentos
    safe_dprintf(sess->control_sock, MSG_501);
    return;
  }

  // Si el archivo no existe respondemos 550 y ni siquiera tocamos el canal de datos.
  if (access(args, R_OK) < 0) {
    safe_dprintf(sess->control_sock, MSG_550, "no existe o sin permisos");
    return;
  }

  // Si hay un canal de datos abierto por un PORT previo, cerrarlo.
  if (sess->data_sock >= 0) {
    close_fd(sess->data_sock, "previous data socket");
    sess->data_sock = -1;
  }

  // Anunciamos al cliente que vamos a abrir el canal de datos.
  safe_dprintf(sess->control_sock, MSG_150);

  // Apertura del canal. OJO: los parentesis abarcan solo la asignacion,
  // la comparacion < 0 va POR FUERA. Si quedan adentro como "fd = (... < 0)"
  // fd termina valiendo 0 o 1 (stdin/stdout), no el FD real del socket.
  if ((fd = dtp_open_active(sess)) < 0) {
    safe_dprintf(sess->control_sock, MSG_425);
    return;
  }

  // Transferencia propiamente dicha. Si se cae a mitad, MSG_451 y limpiamos
  // el canal con dtp_close para no dejar el socket colgado en la sesion
  if (dtp_send_file(fd, (char*)args) < 0) {
    safe_dprintf(sess->control_sock, MSG_451);
    dtp_close(sess);
    return;
  }

  // Transferencia OK: cerramos el canal de datos (esto le indica EOF al
  // cliente) y recien ahi mandamos el 226 final por el canal de control.
  dtp_close(sess);
  safe_dprintf(sess->control_sock, MSG_226);
}

// handle_STOR se encarga de guardar el fichero que se le pasa como argumento
void handle_STOR(const char* args) {
  ftp_session_t* sess = session_get();
  (void)args;
  (void)sess;

  // Placeholder
}

// handle_NOOP se encarga de hacer un "ping" al servidor (NOOP es no-operation)
void handle_NOOP(const char* args) {
  ftp_session_t* sess = session_get();
  (void)args;
  (void)sess;

  // Placeholder
}
