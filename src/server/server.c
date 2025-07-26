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
#include <pthread.h>
#include "../../include/server.h"
#include "../../include/coroutine.h"

#define NUM_WORKERS 4
static Worker workers[NUM_WORKERS];
static int current_worker = 0;

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

void* worker_loop(void* arg)
{
  Worker* w = (Worker *) arg;
  
  // Declare structures for client data
  struct sockaddr_storage remote_addr;     // client address information
  socklen_t sin_size = sizeof remote_addr; // size of client address information
  char remote_ip[INET6_ADDRSTRLEN];        // client ip information

  // Create epoll instance
  struct epoll_event events[MAX_EVENTS];
  // int epfd = epoll_create1(0);
  // if (epfd == -1) {
  //   perror("epoll_create1");
  //   exit(EXIT_FAILURE);
  // }

  // Main Event loop
  printf("Initialzing Thread %d\n", w->id);
  while(1)
  {
    // Pause thread until an event occurs on a socket
    int nfds = epoll_wait(w->epfd, events, MAX_EVENTS, -1);
    if (nfds == -1) 
    {
      perror("epoll_wait");
      exit(EXIT_FAILURE);
    }

    // Once event occurs on one or more sockets, loop through the returned fds
    for(int i = 0; i < nfds; i++) 
    {
      int fd = events[i].data.fd; // get fd
      printf("fd is %d\n", fd);
      uint32_t event = events[i].events; // get event

      // Notification pipe readable: new fd(s) handed over by main thread
      if (fd == w->notify_fds[0]) {
        int client_fd;
        ssize_t n = read(fd, &client_fd, sizeof(int));
        printf("%d fd registering\n", client_fd);
        if (n != sizeof(int)) {
            fprintf(stderr, "Partial read from pipe (%ld bytes)\n", n);
        }

        struct epoll_event ev = {
          .data.fd = client_fd,
          .events = EPOLLIN | EPOLLOUT | EPOLLRDHUP
        };
        epoll_ctl(w->epfd, EPOLL_CTL_ADD, client_fd, &ev);

        make_socket_nonblocking(client_fd);

        int* pfd = malloc(sizeof(int));
        *pfd = client_fd;
        Coroutine* co = coroutine_create(echo_coroutine, pfd, w);
        add_to_ready(w, co);

        continue;
      }

          puts("This prints");
      // Whether we are reading/writing/disconnecting we want to retrieve client information
      // We may need it (e.g. for logging)
      if (getpeername(fd, (struct sockaddr *)&remote_addr, &sin_size) == -1) 
      {
        perror("getpeername");
        continue;
      }
      inet_ntop(remote_addr.ss_family, get_in_addr((struct sockaddr*)&remote_addr), remote_ip, INET6_ADDRSTRLEN);
          puts("This prints");

      // fd is disconnecting
      if (event & EPOLLRDHUP)
      {
        // Connection closed by client
        printf("[-] Client disconnected (%s:%d) from socket %d\n", 
             remote_ip, 
             ntohs(((struct sockaddr_in*)&remote_addr)->sin_port), 
             fd);

        close(fd); // Bye!

        Coroutine* co = w->fd_table[fd];  // get coroutine
        w->fd_table[fd] = NULL;           // remove coroutine from waiting table
        if (co) coroutine_destroy(w, co); // destroy coroutine

        epoll_ctl(w->epfd, EPOLL_CTL_DEL, fd, NULL);
        continue;
      }

      if (w->fd_table[fd])
      {
        Coroutine* co = w->fd_table[fd];

        if ((event & EPOLLIN && co->wait_type == WAIT_READ) || // fd is readable
        (event & EPOLLOUT && co->wait_type == WAIT_WRITE))     // fd is writable
        {
          w->fd_table[fd] = NULL;
          add_to_ready(w, co);
        }
      }
    } 

    // Continue to schedule ready coroutines
    schedule(w);
  }
}

/* Initialize all the worker threads */
void start_workers()
{
  struct epoll_event ev;
  for (int i = 0; i < NUM_WORKERS; i++)
  {
    int epfd = epoll_create1(0);
    if (epfd == -1) {
      perror("epoll_create1");
      exit(EXIT_FAILURE);
    }

    int pipefds[2];
    if (pipe(pipefds) == -1) {
      perror("pipe");
      exit(EXIT_FAILURE);
    }

    workers[i] = (Worker){
      .epfd = epfd,
      .notify_fds = { pipefds[0], pipefds[1] },
      .id = i,
      .ready_queue = NULL,
      .current = NULL
    };

    // register notfy pipe in epoll
    ev.data.fd = workers[i].notify_fds[0];
    ev.events = EPOLLIN;
    epoll_ctl(workers[i].epfd, EPOLL_CTL_ADD, workers[i].notify_fds[0], &ev);

    pthread_create(&workers[i].thread, NULL, worker_loop, &workers[i]);
  }
}

void server_init(const char *port)
{
  // Declare structures for client data
  struct sockaddr_storage remote_addr;     // client address information
  socklen_t sin_size = sizeof remote_addr; // size of client address information
  char remote_ip[INET6_ADDRSTRLEN];        // client ip information

  // Create listener socket descriptor
  int sockfd = create_listener_socket(port, BACKLOG); 

  // Start workers
  start_workers();

  // Accept() loop
  printf("[*] Server listening on port %s\n", port);
  while (1)
  {
    int new_fd = accept(sockfd, (struct sockaddr *)&remote_addr, &sin_size); // accept new connection
    if (new_fd == -1) 
    {
      // perror("accept");
      continue;
    }

    inet_ntop(remote_addr.ss_family, get_in_addr((struct sockaddr*)&remote_addr), remote_ip, INET6_ADDRSTRLEN);

    make_socket_nonblocking(new_fd);

    printf("[+] New client connection (%s:%d) on socket %d\n", 
           remote_ip, 
           ntohs(((struct sockaddr_in*)&remote_addr)->sin_port), 
           new_fd);

    // Sends a message to a target worker thread via a pipe containing the new clients socket
    // The worker thread registers the socket and polls for writes/reads
    int target = current_worker++ % NUM_WORKERS; // Hashing function
    printf("Targetting thread %d\n", target);
    write(workers[target].notify_fds[1], &new_fd, sizeof(int));
  }
  
  // Register the listener socket on epoll
  // struct epoll_event ev;
  // ev.data.fd = sockfd;
  // ev.events = EPOLLIN; // We only care about if the listener is readable (EPOLLIN)
  // if (epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev) == -1) {
  //   perror("epoll_ctl: listener");
  //   exit(EXIT_FAILURE);
  // }

}
