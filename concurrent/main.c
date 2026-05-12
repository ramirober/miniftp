#include <arpa/inet.h>  // for inet_ntoa()
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>  // EXIT_*
#include <string.h>
#include <syslog.h>
#include <unistd.h>  // for close(), daemon()

#include "arguments.h"
#include "server.h"
#include "signals.h"
#include "utils.h"

int main(int argc, char** argv) {
  struct arguments args;

  if (parse_arguments(argc, argv, &args) != 0) return EXIT_FAILURE;

  // server_init se inicia ANTES que daemon() para que los errores de configuración (puerto
  // incorrecto, dirección ocupada) sean visibles en la terminal en lugar de ser silenciados por
  // /dev/null.
  int listen_fd = server_init(args.address, args.port);
  if (listen_fd < 0) return EXIT_FAILURE;

  if (args.daemon_mode) {
    // daemon(nochdir=0, noclose=0):
    //   fork() → padre existe, hijo continua
    //   setsid() → nueva sesion, sin terminal asociada
    //   chdir("/") → evita bloqueos en sistemas de archivos montados
    //   stdin/stdout/stderr → /dev/null
    if (daemon(0, 0) < 0) {
      perror("daemon");
      return EXIT_FAILURE;
    }
    daemon_mode = 1;
    openlog(APP_NAME, LOG_PID, LOG_DAEMON);
  }

  log_msg(LOG_INFO, "Server started on %s:%d", args.address, args.port);

  // setup_signals() debe correr DESPUES de daemon() porque llama setpgid(0,0)
  // para hacer que el proceso (ahora daemonizado) sea el lider del grupo.
  setup_signals();

  while (1) {
    struct sockaddr_in client_addr;

    memset(&client_addr, 0, sizeof(client_addr));
    int new_socket = server_accept(listen_fd, &client_addr);
    if (new_socket < 0) {
      continue;
    }

    pid_t pid = fork();
    if (pid < 0) {
      log_msg(LOG_ERR, "fork failed: %s", strerror(errno));
      close_fd(new_socket, "client (fork failed)");
      continue;
    }

    if (pid == 0) {
      // Child process

      // Join parent's PGID
      pid_t pgid = getpgrp();  // parent's PGID
      if (setpgid(0, pgid) < 0) {
        log_msg(LOG_ERR, "setpgid child: %s", strerror(errno));
      }
      log_msg(LOG_DEBUG, "Child PID %d PGID %d", getpid(), getpgrp());

      setup_child_signals();

      close(listen_fd);  // Don't need the listener

      char client_ip[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
      log_msg(LOG_INFO, "[+] New connection from %s:%d handled by child PID %d", client_ip,
              ntohs(client_addr.sin_port), getpid());

      server_loop(new_socket);  // Each child sets its own session

      log_msg(LOG_INFO, "[-] Child PID %d closing connection for %s:%d", getpid(), client_ip,
              ntohs(client_addr.sin_port));

      // https://en.cppreference.com/w/c/program/EXIT_status
      exit(EXIT_SUCCESS);
    } else {
      // Parent process
      close_fd(new_socket, "client socket");  // important to avoid socket leaks
    }
  }

  // NEVER GO HERE
  close_fd(listen_fd, "listening socket");
  // Cerramos el log si estamos en modo daemon
  if (args.daemon_mode) closelog();

  // https://en.cppreference.com/w/c/program/EXIT_status
  return EXIT_SUCCESS;
}
