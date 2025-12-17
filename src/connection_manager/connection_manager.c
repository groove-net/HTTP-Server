#include "../../include/connection_manager.h"
#include <arpa/inet.h> // Added
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h> // Added
#include <sys/wait.h>
#include <unistd.h>

// make fd non-blokcing
int make_socket_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa) {
  if (sa->sa_family == AF_INET) {
    return &(((struct sockaddr_in *)sa)->sin_addr);
  }

  return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

// create and return a listening socket
int create_listener_socket(const char *port, int backlog) {
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

  if (listen(sockfd, backlog) == -1) {
    perror("listen");
    exit(EXIT_FAILURE);
  }

  // Make non blocking
  make_socket_nonblocking(sockfd);

  return sockfd;
}

void close_connection(int fd, Worker *w) {
  // Declare structures for client data
  struct sockaddr_storage remote_addr;     // client address information
  socklen_t sin_size = sizeof remote_addr; // size of client address information
  char remote_ip[INET6_ADDRSTRLEN];        // client ip information

  // Retrieve client info for logging
  if (getpeername(fd, (struct sockaddr *)&remote_addr, &sin_size) == -1) {
    perror("getpeername");
    return;
  }
  inet_ntop(remote_addr.ss_family, get_in_addr((struct sockaddr *)&remote_addr),
            remote_ip, INET6_ADDRSTRLEN);

  shutdown(fd, SHUT_WR); // Bye!
  close(fd);

  // Get the head of the FdNode list for this FD
  FdNode *node = w->fd_table[fd];
  // Check if the FdNode pointer itself is NULL before dereferencing it
  if (node) {
    // If an FdNode exists, proceed with cleanup
    Coroutine *co = w->fd_table[fd]->co; // get coroutine
    w->fd_table[fd] = NULL;              // remove coroutine from waiting table
    if (co)
      coroutine_destroy(w, co); // destroy coroutine
  }

  epoll_ctl(w->epfd, EPOLL_CTL_DEL, fd, NULL);

  // Log
  printf("[-] Client disconnected (%s:%d) from socket %d\n", remote_ip,
         ntohs(((struct sockaddr_in *)&remote_addr)->sin_port), fd);
}
