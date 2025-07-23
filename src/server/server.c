
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include "../../include/server.h"

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

// return listening socket
int create_listener_socket(const char *port, int backlog)
{
  struct addrinfo hints, *servinfo;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE; // use my IP

  int rv = getaddrinfo(NULL, port, &hints, &servinfo);
  if (rv)
  {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
    exit(EXIT_FAILURE);
  }

  int sockfd;
  int yes = 1;
  struct addrinfo *p;
  // loop through all the results and bind to the first we can
  for (p = servinfo; p != NULL; p = p->ai_next)
  {
    sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (sockfd == -1)
    {
      perror("server: socket");
      continue;
    }
    
    
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
    {
      perror("setsockopt");
      exit(EXIT_FAILURE);
    }

    if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1)
    {
      close(sockfd);
      perror("server: bind");
      continue;
    }

    break;
  }

  freeaddrinfo(servinfo); // all done with this structure

  if (p == NULL)
  {
    fprintf(stderr, "server: failed to bind\n");
    exit(EXIT_FAILURE);
  }

  if (listen(sockfd, backlog) == -1)
  {
    perror("listen");
    exit(EXIT_FAILURE);
  }

  // Make non blocking
  int flags = fcntl(sockfd, F_GETFL, 0);
  fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

  return sockfd;
}

void server_init(const char *port)
{
  int sockfd = create_listener_socket(port, BACKLOG); // create listening socket descriptor
  printf("[*] Server listening on port %s\n", port);
  struct sockaddr_storage remote_addr; // client address information
  int new_fd; // new connection
  socklen_t sin_size;
  char buf[16]; // buffer for client data
  char remote_ip[INET6_ADDRSTRLEN];

  // create epoll instance
  int epfd = epoll_create1(0);
  if (epfd == -1) {
    perror("epoll_create1");
    exit(1);
  }

  struct epoll_event ev, events[MAX_EVENTS];
  ev.events = EPOLLIN;
  ev.data.fd = sockfd;

  if (epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev) == -1) {
    perror("epoll_ctl: listener");
    exit(1);
  }

  // accept() loop
  while(1)
  {
    int poll_count = epoll_wait(epfd, events, MAX_EVENTS, -1);

    if (poll_count == -1) {
      perror("epoll_wait");
      exit(1);
    }

    // Run through the existing connections looking for data to read
    for(int i = 0; i < poll_count; i++) {
      int fd = events[i].data.fd;

      // Check if someone's ready to read
      if (fd == sockfd) {
        // If listener is ready to read, handle new connection

        sin_size = sizeof remote_addr;
        new_fd = accept(sockfd, (struct sockaddr *)&remote_addr, &sin_size);

        if (new_fd == -1) {
          perror("accept");
          continue;
        }

        inet_ntop(remote_addr.ss_family, get_in_addr((struct sockaddr*)&remote_addr), remote_ip, INET6_ADDRSTRLEN);

        int flags = fcntl(new_fd, F_GETFL, 0);
        fcntl(new_fd, F_SETFL, flags | O_NONBLOCK);

        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.fd = new_fd;

        if (epoll_ctl(epfd, EPOLL_CTL_ADD, new_fd, &ev) == -1) {
            perror("epoll_ctl: new connection");
            close(new_fd);
            continue;
        }

        printf("[+] New client connection (%s:%d) on socket %d\n", 
               remote_ip, 
               ntohs(((struct sockaddr_in*)&remote_addr)->sin_port), 
               new_fd);
      } else {
        // If not the listener, we're just a regular client
        int nbytes = recv(fd, buf, sizeof buf, 0);

        if (getpeername(fd, (struct sockaddr *)&remote_addr, &sin_size) == -1) {
          perror("getpeername");
          continue;
        }

        inet_ntop(remote_addr.ss_family, get_in_addr((struct sockaddr*)&remote_addr), remote_ip, INET6_ADDRSTRLEN);

        if (nbytes <= 0) {
          // Got error
          if (nbytes < 0) perror("recv");

          // Connection closed by client
          printf("[-] Client disconnected (%s:%d) from socket %d\n", 
               remote_ip, 
               ntohs(((struct sockaddr_in*)&remote_addr)->sin_port), 
               fd);

          close(fd); // Bye!
          epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        } else {
          // We got some good data from a client
          printf("[*] Received %d bytes from socket %d: %.*s", nbytes, fd, nbytes, buf);
          if (buf[nbytes-1] != '\n') printf("\n");
        }
      } // END handle data from client
    } // END looping through file descriptors
  }
}
