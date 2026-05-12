#include "arguments.h"

#include <argp.h>
#include <arpa/inet.h>
#include <errno.h>  // perror()
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  // for geteuid()

#include "config.h"

static struct argp_option options[] = {
    {"port", 'p', "PORT", 0, PORT_DOC, 0},
    {"address", 'a', "ADDR", 0, ADDR_DOC, 0},
    {"daemon", 'd', NULL, 0, "Run as a background daemon (logs via syslog)", 0},
    {0}};

// --- CLI options ---
const char* argp_program_version = "server " VERSION;
const char* argp_program_bug_address = BUG_EMAIL;
static char doc[] = "Simple FTP server";
static char args_doc[] = "[PORT [ADDRESS]]";

static int is_valid_local_ip(const char* ip_str) {
  struct ifaddrs *ifaddr, *ifa;
  int found = 0;

  if (getifaddrs(&ifaddr) == -1) {
    perror("getifaddrs");
    return 0;
  }

  for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
      char addr_buf[INET_ADDRSTRLEN];
      struct sockaddr_in* in_addr = (struct sockaddr_in*)ifa->ifa_addr;
      inet_ntop(AF_INET, &in_addr->sin_addr, addr_buf, sizeof(addr_buf));
      if (strcmp(ip_str, addr_buf) == 0) {
        found = 1;
        break;
      }
    }
  }

  freeifaddrs(ifaddr);
  return found;
}

// --- Argument parser ---
static error_t parse_opt(int key, char* arg, struct argp_state* state) {
  struct arguments* args = state->input;

  switch (key) {
    case 'p':
      args->port = atoi(arg);
      args->port_set = 1;
      break;
    case 'a':
      strncpy(args->address, arg, INET_ADDRSTRLEN);
      args->address[INET_ADDRSTRLEN - 1] = '\0';
      args->address_set = 1;
      break;
    case 'd':
      args->daemon_mode = 1;
      break;
    case ARGP_KEY_ARG:
      if (state->arg_num == 0 && !args->port_set) {
        args->port = atoi(arg);
        args->port_set = 1;
      } else if (state->arg_num == 1 && !args->address_set) {
        strncpy(args->address, arg, INET_ADDRSTRLEN);
        args->address[INET_ADDRSTRLEN - 1] = '\0';
        args->address_set = 1;
      } else {
        argp_usage(state);  // Too many args
      }
      break;
    case ARGP_KEY_END:
      if (args->port == 21 && geteuid() != 0) {
        argp_error(state, "Permission denied: Port 21 requires root privileges");
      }
      if (args->port != 21 && (args->port < IPPORT_RESERVED || args->port > IPPORT_USERRESERVED)) {
        argp_error(state, "Invalid port: %d. Allowed: 21 (as root), or %d–%d", args->port,
                   IPPORT_RESERVED, IPPORT_USERRESERVED);
      }
      if (!is_valid_local_ip(args->address)) {
        argp_error(state, "Invalid IP: %s is not a local interface", args->address);
      }
      break;
    default:
      return ARGP_ERR_UNKNOWN;
  }

  return 0;
}

// --- Argp parser definition ---
static struct argp argp = {options, parse_opt, args_doc, doc, NULL, NULL, NULL};

// Call this from main:
int parse_arguments(int argc, char** argv, struct arguments* args) {
  // Initialize defaults:
  args->port = FTP_PORT;
  args->port_set = 0;
  strncpy(args->address, LOCALHOST, INET_ADDRSTRLEN);
  args->address[INET_ADDRSTRLEN - 1] = '\0';
  args->address_set = 0;
  args->daemon_mode = 0;

  return argp_parse(&argp, argc, argv, 0, NULL, args);
}
