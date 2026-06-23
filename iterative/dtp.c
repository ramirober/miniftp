#define _GNU_SOURCE
#include "dtp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "config.h"
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
  // sess es una variable declarada en donde se ejecute dtp_open_active
  // Y la referenciamos en session.c
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

// dtp_send_file manda el contenido del archivo pedido por el socket de
// datos que entrego dtp_open_active(). Devuelve 0 si la transferencia se
// completa hasta EOF, y -1 si falla la apertura del archivo, el read() o el
// write() al socket.

int dtp_send_file(int data_fd, char* path) {
  if (data_fd < 0 || !path || *path == '\0') return -1;

  // Abrimos en solo lectura. FTP por defecto trabaja en modo binario
  int file_fd = open(path, O_RDONLY);
  if (file_fd < 0) {
    return -1;
  }

  // Chequeamos que sea archivo regular. Si nos piden RETR sobre un directorio
  // o un dispositivo, abortamos antes de empezar a mandar basura por el socket.
  struct stat st;
  // Checkeamos con fstat() que sea un archivo regular o que S_ISREG() retorne
  // true
  if (fstat(file_fd, &st) < 0 || !S_ISREG(st.st_mode)) {
    log_msg(LOG_ERR, "dtp_send_file: '%s' no es un archivo regular", path);
    close(file_fd);
    return -1;
  }

  // Loop de transferencia: leemos del archivo y escribimos al socket en
  // bloques de BUFFER_SIZE bytes. Cortamos cuando read() devuelve 0 (EOF).
  char buf[BUFFER_SIZE];
  ssize_t n;
  while ((n = read(file_fd, buf, sizeof(buf))) > 0) {
    // write() en un socket puede escribir menos bytes de los pedidos
    // (escritura parcial). Loopeamos hasta vaciar el buffer leido o hasta
    // que una escritura falle de verdad.
    ssize_t written = 0;
    while (written < n) {
      ssize_t w = write(data_fd, buf + written, (size_t)(n - written));
      if (w < 0) {
        if (errno == EINTR) continue;
        log_msg(LOG_ERR, "dtp_send_file: write fallo: %s", strerror(errno));
        close(file_fd);
        return -1;
      }
      written += w;
    }
  }

  // n < 0 indica error de lectura del archivo (no EOF, que seria n == 0)
  if (n < 0) {
    log_msg(LOG_ERR, "dtp_send_file: read fallo: %s", strerror(errno));
    close(file_fd);
    return -1;
  }

  close(file_fd);
  return 0;
}

// dtp_recv_file es el espejo de dtp_send_file: recibe bytes desde el socket de
// datos que entrego dtp_open_active() y los escribe al archivo destino.
// Devuelve 0 si la transferencia se completa hasta EOF (el cliente cierra su
// extremo del canal), y -1 si falla la apertura/creacion del archivo o el
// write() local. Abre con O_TRUNC: si el archivo ya existe, lo sobrescribe.
int dtp_recv_file(int data_fd, char* path) {
  if (data_fd < 0 || !path || *path == '\0') return -1;

  // O_CREAT lo crea si no existe; O_TRUNC lo vacia si ya existia (sobrescribe).
  // Permisos 0644 (rw-r--r--), sujetos al umask del proceso.
  int file_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (file_fd < 0) {
    log_msg(LOG_ERR, "dtp_recv_file: no se pudo abrir '%s': %s", path, strerror(errno));
    return -1;
  }

  // Loop de recepcion: leemos del socket y escribimos al archivo en bloques de
  // BUFFER_SIZE bytes. Cortamos cuando read() devuelve 0 (el cliente cerro el
  // canal de datos = fin de la transferencia).
  char buf[BUFFER_SIZE];
  ssize_t n;
  while ((n = read(data_fd, buf, sizeof(buf))) > 0) {
    // write() puede escribir menos bytes de los pedidos; loopeamos hasta
    // vaciar el buffer o hasta que una escritura falle de verdad.
    ssize_t written = 0;
    while (written < n) {
      ssize_t w = write(file_fd, buf + written, (size_t)(n - written));
      if (w < 0) {
        if (errno == EINTR) continue;
        log_msg(LOG_ERR, "dtp_recv_file: write fallo: %s", strerror(errno));
        close(file_fd);
        return -1;
      }
      written += w;
    }
  }

  // n < 0 indica error de lectura del socket (no EOF, que seria n == 0)
  if (n < 0) {
    log_msg(LOG_ERR, "dtp_recv_file: read fallo: %s", strerror(errno));
    close(file_fd);
    return -1;
  }

  close(file_fd);
  return 0;
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
