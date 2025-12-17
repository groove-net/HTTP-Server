#include "../../include/server.h"
#include "../../include/connection_manager.h"
#include "../../include/request-handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

#define NUM_WORKERS 4
static Worker workers[NUM_WORKERS];
static int current_worker = 0;

void *worker_loop(void *arg) {
  // Create epoll instance
  int epfd = epoll_create1(0);
  if (epfd == -1) {
    perror("epoll_create1");
    exit(EXIT_FAILURE);
  }

  // Initialize worker struct
  Worker *w = (Worker *)arg;
  if (pthread_mutex_init(&w->ready_mutex, NULL) != 0) {
    perror("ready_mutex init failed");
    exit(EXIT_FAILURE);
  }
  if (pthread_mutex_init(&w->fd_mutex, NULL) != 0) {
    perror("fd_mutex init failed");
    exit(EXIT_FAILURE);
  }
  w->epfd = epfd;
  if (pipe(w->notify_fds) == -1) {
    perror("pipe");
    exit(EXIT_FAILURE);
  }
  // make the pipe's READ end non-blocking
  if (make_socket_nonblocking(w->notify_fds[0]) == -1) {
    perror("make_socket_nonblocking pipe failed");
    exit(EXIT_FAILURE);
  }
  w->ready_head = NULL; // head of ready queue
  w->ready_tail = NULL; // tail of ready queue (used for FIFO)
  w->current = NULL;

  // Register notfy pipe in epoll
  struct epoll_event ev;
  ev.data.fd = w->notify_fds[0];
  ev.events = EPOLLIN;
  epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->notify_fds[0], &ev);

  // Main Event loop
  // printf("Initialzing Thread %d\n", w->id);
  struct epoll_event events[MAX_EVENTS];
  while (1) {
    // Pause thread until an event occurs on a socket
    int nfds = epoll_wait(w->epfd, events, MAX_EVENTS, -1);
    if (nfds == -1) {
      perror("epoll_wait");
      exit(EXIT_FAILURE);
    }

    // Once event occurs on one or more sockets, loop through the returned fds
    for (int i = 0; i < nfds; i++) {
      int fd = events[i].data.fd;        // get fd
      uint32_t event = events[i].events; // get event

      // Notification pipe readable: new client fd(s) handed over by main thread
      if (fd == w->notify_fds[0]) {
        // Drain all queued fds
        while (1) {
          int client_fd;
          if (read(fd, &client_fd, sizeof(int)) <= 0)
            break;

          struct epoll_event ev = {.data.fd = client_fd,
                                   .events = EPOLLIN | EPOLLOUT | EPOLLRDHUP};
          if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
            perror("epoll_ctl ADD failed");
            printf("client_fd %d failed to be added to the event "
                   "loop.\n",
                   client_fd);
            exit(EXIT_FAILURE);
          }

          // Pass new fd to corotuine
          int *pfd = malloc(sizeof(int));
          *pfd = client_fd;

          Coroutine *co = coroutine_create(entry, pfd, w);
          add_to_ready(w, co);
        }
        continue;
      }

      // fd is disconnecting
      if (event & EPOLLRDHUP) {
        wake_fd(w, fd); // free nodes + requeue coroutines
        close_connection(fd, w);
        continue;
      }

      // fd readable/writable
      if ((event & EPOLLIN) || (event & EPOLLOUT)) {
        wake_fd(w, fd); // thread-safe, clears everything properly
      }
    }

    // Continue to schedule ready coroutines
    schedule(w);
  }
}

/* Initialize worker threads for perform the worker loop*/
void start_workers(void) {
  for (int i = 0; i < NUM_WORKERS; i++) {
    workers[i].id = i;
    pthread_create(&workers[i].thread, NULL, worker_loop, &workers[i]);
  }
}

void server_init(const char *port) {
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
  while (1) {
    int new_fd = accept(sockfd, (struct sockaddr *)&remote_addr,
                        &sin_size); // accept new connection
    if (new_fd == -1) {
      // perror("accept");
      continue;
    }

    inet_ntop(remote_addr.ss_family,
              get_in_addr((struct sockaddr *)&remote_addr), remote_ip,
              INET6_ADDRSTRLEN);

    make_socket_nonblocking(new_fd);

    // Sends a message to a target worker thread via a pipe containing the new
    // clients socket. The worker thread registers the socket and polls for
    // writes/reads
    int target = current_worker++ % NUM_WORKERS; // Hashing function
    printf("[+] New client connection (%s:%d) on socket %d, thread %d\n",
           remote_ip, ntohs(((struct sockaddr_in *)&remote_addr)->sin_port),
           new_fd, target);
    write(workers[target].notify_fds[1], &new_fd, sizeof(int));
  }
}
