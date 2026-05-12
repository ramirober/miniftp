#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "pi.h"
#include "session.h"
#include "utils.h"

extern int server_socket;

int server_init(const char* ip, int port) {
  struct sockaddr_in server_addr;

  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    fprintf(stderr, "Error creating socket: ");
    perror(NULL);
    return -1;
  }

  // avoid problem with reuse inmeditely after force quiting
  const int opt = 1;
  if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    fprintf(stderr, "Error setting SO_REUSEADDR: ");
    perror(NULL);
    close(listen_fd);
    return -1;
  }

#ifdef SO_REUSEPORT
  if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
    fprintf(stderr, "Error setting SO_REUSEPORT: ");
    perror(NULL);
    close(listen_fd);
    return -1;
  }
#endif

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);

  if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
    fprintf(stderr, "Invalid IP address: %s\n", ip);
    close(listen_fd);
    return -1;
  }

  if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    fprintf(stderr, "Bind failed: ");
    perror(NULL);
    close(listen_fd);
    return -1;
  }

  char ip_buf[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &server_addr.sin_addr, ip_buf, sizeof(ip_buf));
  printf("Listening on %s:%d\n", ip_buf, port);

  if (listen(listen_fd, SOMAXCONN) < 0) {
    fprintf(stderr, "Listen failed: ");
    perror(NULL);
    close(listen_fd);
    return -1;
  }

  server_socket = listen_fd;
  return listen_fd;
}

int server_accept(int listen_fd, struct sockaddr_in* client_addr) {
  // client_addr can be NULL if caller doesn't need client info
  socklen_t addrlen = sizeof(*client_addr);
  int new_socket = accept(listen_fd, (struct sockaddr*)client_addr, &addrlen);

  // EINTR for avoid errors by signal reentry
  // https://stackoverflow.com/questions/41474299/checking-if-errno-eintr-what-does-it-mean
  if (new_socket < 0 && errno != EINTR) {
    log_msg(LOG_ERR, "Accept failed: %s", strerror(errno));
    return -1;
  }

  return new_socket;
}

void server_loop(int socket) {
  // Establish the session socket with the client
  session_init(socket);

  // Send initial FTP welcome message
  if (welcome(current_sess) < 0) return;

  while (1) {
    // Get command from Control Channel
    if (getexe_command(current_sess) < 0) break;
  }

  session_cleanup();
}
