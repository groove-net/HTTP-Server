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

// make fd non-blokcing
int make_socket_nonblocking(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

// create and return a listening socket
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
  make_socket_nonblocking(sockfd);

  return sockfd;
}

void server_init(const char *port)
{
  // Declare structures for client data
  struct sockaddr_storage remote_addr;     // client address information
  socklen_t sin_size = sizeof remote_addr; // size of client address information
  char remote_ip[INET6_ADDRSTRLEN];        // client ip information
  char buf[16];                            // buffer for client data

  // Create epoll instance
  struct epoll_event events[MAX_EVENTS];
  int epfd = epoll_create1(0);
  if (epfd == -1) {
    perror("epoll_create1");
    exit(EXIT_FAILURE);
  }

  // Create listener socket descriptor
  int sockfd = create_listener_socket(port, BACKLOG); 
  
  // Register the listener socket on epoll
  struct epoll_event ev;
  ev.data.fd = sockfd;
  ev.events = EPOLLIN; // We only care about if the listener is readable (EPOLLIN)
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev) == -1) {
    perror("epoll_ctl: listener");
    exit(EXIT_FAILURE);
  }

  // Main Event loop
  printf("[*] Server listening on port %s\n", port);
  while(1)
  {
    // Pause thread until an event occurs on a socket
    int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
    if (nfds == -1) 
    {
      perror("epoll_wait");
      exit(EXIT_FAILURE);
    }

    // Once event occurs on one or more sockets, loop through the returned fds
    for(int i = 0; i < nfds; i++) 
    {
      int fd = events[i].data.fd; // get fd
      uint32_t event = events[i].events; // get event

      // fd is the listener
      if (fd == sockfd) 
      {
        // fd is readable
        if (event & EPOLLIN)
        {
          // The listener is ready to read, this means we need to handle a new connection
          int new_fd = accept(sockfd, (struct sockaddr *)&remote_addr, &sin_size); // accept new connection
          if (new_fd == -1) 
          {
            perror("accept");
            continue;
          }

          inet_ntop(remote_addr.ss_family, get_in_addr((struct sockaddr*)&remote_addr), remote_ip, INET6_ADDRSTRLEN);

          make_socket_nonblocking(new_fd);

          // Register new socket on epoll
          ev.data.fd = new_fd;
          ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;  // We want to check for read(EPOLLIN), 
                                                        // write(EPOLLOUT) and 
                                                        // hang up/peer closed connection (EPOLLRDHUP)
          if (epoll_ctl(epfd, EPOLL_CTL_ADD, new_fd, &ev) == -1) 
          {
              perror("epoll_ctl: new connection");
              close(new_fd);
              continue;
          }

          // All done! Now you may do some work in post
          printf("[+] New client connection (%s:%d) on socket %d\n", 
                 remote_ip, 
                 ntohs(((struct sockaddr_in*)&remote_addr)->sin_port), 
                 new_fd);
        }
      } 
      // fd is not the listener, just a regular client
      else
      {
        // Whether we are reading/writing/disconnecting we want to retrieve client information
        // We may need it (e.g. for logging)
        if (getpeername(fd, (struct sockaddr *)&remote_addr, &sin_size) == -1) 
        {
          perror("getpeername");
          continue;
        }

        inet_ntop(remote_addr.ss_family, get_in_addr((struct sockaddr*)&remote_addr), remote_ip, INET6_ADDRSTRLEN);

        // fd is disconnecting
        if (event & EPOLLRDHUP)
        {
          // Connection closed by client
          printf("[-] Client disconnected (%s:%d) from socket %d\n", 
               remote_ip, 
               ntohs(((struct sockaddr_in*)&remote_addr)->sin_port), 
               fd);

          close(fd); // Bye!
          epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
          continue;
        }

        // fd is readable
        if (event & EPOLLIN)
        {
          int nbytes = recv(fd, buf, sizeof buf, 0);
          if (nbytes < 0) perror("recv");
          // We got some good data from a client
          printf("[*] Received %d bytes from socket %d: %.*s", nbytes, fd, nbytes, buf);
          if (buf[nbytes-1] != '\n') printf("\n");
        }

        // fd is writable
        if (event & EPOLLOUT)
        {
          continue;
        }
      } 
    } 
  }
}
