#define _GNU_SOURCE
#include "dtp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "utils.h"

int check_credentials(char* user, char* pass) {
  FILE* file;
  char *path = PWDFILE, *line = NULL, cred[100];
  size_t len = 0;
  int found = -1;

  // make the credential string
  sprintf(cred, "%s:%s", user, pass);

  // check if it is present in any ftpusers line
  file = fopen(path, "r");
  if (file == NULL) {
    fprintf(stderr, "Error: no se pudo abrir el archivo de usuarios.\n");
    return -1;
  }

  while (getline(&line, &len, file) != -1) {
    strtok(line, "\n");
    if (strcmp(line, cred) == 0) {
      found = 0;
      break;
    }
  }

  fclose(file);
  if (line) free(line);
  return found;
}

// Funcion para abrir la conexion
int dtp_open_active(ftp_session_t* sess) {
  if (!sess) return -1;

  // sin_port == 0 marca "PORT aún no recibido en esta sesion"
  if (sess->data_addr.sin_port == 0) {
    log_msg(LOG_ERR, "dtp_open_active: PORT aun no recibido en esta sesion");
    return -1;
  }

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  // Si el socket no pudo ser creado, no podemos continuar
  if (fd < 0) {
    log_msg(LOG_ERR, "dtp socket: %s", strerror(errno));
    return -1;
  }

  // El servidor inicia la conexion desde el "FTP-DATA port",
  // tradicionalmente el 20. Solamente asignamos el 20 si tenemos privilegios;
  // en caso contrario el kernel asigna un puerto efimero al hacer connect().
  if (geteuid() == 0) {
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in src;
    // memset crea un buffer de ceros, asignamos el puerto a 0 para que el kernel
    // elija uno
    memset(&src, 0, sizeof(src));
    src.sin_family = AF_INET;
    // Establecemos el puerto a 20 para que el kernel elija uno
    src.sin_port = htons(20);
    // htonl convierte un int a long, asignamos la direccion a 0.0.0.0 para que el
    // kernel elija una
    src.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr*)&src, sizeof(src)) < 0) {
      log_msg(LOG_WARNING, "bind dtp al puerto 20 fallo: %s (usando fallback de puerto efimero)",
              strerror(errno));
      // no es fatal: seguimos al connect() y dejamos que el kernel resuelva
    }
  }

  if (connect(fd, (struct sockaddr*)&sess->data_addr, sizeof(sess->data_addr)) < 0) {
    log_msg(LOG_ERR, "dtp conectado: %s", strerror(errno));
    close(fd);
    return -1;
  }

  // Si todo sale bien, escribimos en el log que se ha conectado
  char ip_buf[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &sess->data_addr.sin_addr, ip_buf, sizeof(ip_buf));
  // pasamos la direccion IP + el puerto al log
  log_msg(LOG_INFO, "Conexion ACTIVA establecida a %s:%u", ip_buf, ntohs(sess->data_addr.sin_port));

  sess->data_sock = fd;
  return fd;
}

// Funcion para cerrar la conexion
void dtp_close(ftp_session_t* sess) {
  if (!sess) return;
  if (sess->data_sock >= 0) {
    close_fd(sess->data_sock, "data socket");
    sess->data_sock = -1;
  }
  memset(&sess->data_addr, 0, sizeof(sess->data_addr));
}
