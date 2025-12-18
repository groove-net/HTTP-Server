#include "../../include/server.h"
#include "../../include/connection_manager.h"
#include "../../include/liblog.h"
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>

#define BACKLOG 10 // Max number of pending connections in the queue

int create_listener_socket(const char *port);

// Initialize Server
void server_init(const char *port) {
  // Declare structures for client data
  struct sockaddr_storage remote_addr;     // client address information
  socklen_t sin_size = sizeof remote_addr; // size of client address information

  // Start worker threads
  cm_init_thread_pool(4);

  // Create listener socket descriptor
  int sockfd = create_listener_socket(port);
  log_info("[*] Server listening on port %s", port);

  // Accept() loop
  while (1) {
    int client_fd = accept(sockfd, (struct sockaddr *)&remote_addr,
                           &sin_size); // accept new connection
    if (client_fd == -1)
      continue;

    // Simple, clean handoff to connection manager
    cm_dispatch_connection(client_fd);
  }
}

// create and return a listening socket
int create_listener_socket(const char *port) {
  struct addrinfo hints, *servinfo;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE; // use my IP

  int rv = getaddrinfo(NULL, port, &hints, &servinfo);
  if (rv) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
    exit(EXIT_FAILURE);
  }

  int sockfd;
  int yes = 1;
  struct addrinfo *p;
  // loop through all the results and bind to the first we can
  for (p = servinfo; p != NULL; p = p->ai_next) {
    sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (sockfd == -1) {
      perror("server: socket");
      continue;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
      perror("setsockopt");
      exit(EXIT_FAILURE);
    }

    if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
      close(sockfd);
      perror("server: bind");
      continue;
    }

    break;
  }

  freeaddrinfo(servinfo); // all done with this structure

  if (p == NULL) {
    fprintf(stderr, "server: failed to bind\n");
    exit(EXIT_FAILURE);
  }

  if (listen(sockfd, BACKLOG) == -1) {
    perror("listen");
    exit(EXIT_FAILURE);
  }

  // Make non blocking
  int flags = fcntl(sockfd, F_GETFL, 0);
  fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

  return sockfd;
}
