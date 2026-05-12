#include <arpa/inet.h>  // for inet_ntoa()
#include <errno.h>
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

  // server_init runs BEFORE daemon() so config errors (bad port, busy address)
  // are visible on the terminal instead of getting swallowed by /dev/null.
  int listen_fd = server_init(args.address, args.port);
  if (listen_fd < 0) return EXIT_FAILURE;

  if (args.daemon_mode) {
    // daemon(nochdir=0, noclose=0):
    //   fork() → padre existe, hijo continúa
    //   setsid() → nueva sesion, sin terminal asociada
    //   chdir("/") → evitar bloqueo de cualquier sistema de archivos montado
    //   stdin/stdout/stderr → /dev/null
    if (daemon(0, 0) < 0) {
      perror("daemon");
      return EXIT_FAILURE;
    }
    daemon_mode = 1;
    openlog(APP_NAME, LOG_PID, LOG_DAEMON);
  }

  log_msg(LOG_INFO, "Server started on %s:%d", args.address, args.port);

  setup_signals();

  while (1) {
    struct sockaddr_in client_addr;
    int new_socket = server_accept(listen_fd, &client_addr);
    if (new_socket < 0) continue;

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    log_msg(LOG_INFO, "Connection from %s:%d accepted", client_ip, ntohs(client_addr.sin_port));

    server_loop(new_socket);

    log_msg(LOG_INFO, "Connection from %s:%d closed", client_ip, ntohs(client_addr.sin_port));
  }

  // NEVER GO HERE
  close_fd(listen_fd, "listening socket");
  if (args.daemon_mode) closelog();

  // https://en.cppreference.com/w/c/program/EXIT_status
  return EXIT_SUCCESS;
}
