#include "../../include/connection_manager.h"
#include "../../include/liblog.h"
#include "../../include/request-handler.h"
#include "async.c"
#include "coroutine.c"
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>

#define DEBUG 0
#define MAX_EVENTS 64 // Max number of events returned by epoll

static Worker *worker_pool = NULL;
static int total_workers = 0;
static int next_worker_idx = 0;

void worker_init(Worker *w);
void *worker_loop(void *arg);

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

void cm_init_thread_pool(int num_workers) {
  total_workers = num_workers;
  worker_pool = malloc(sizeof(Worker) * num_workers);

  for (int i = 0; i < num_workers; i++) {
    // Initialize worker internals (epoll, pipes, mutexes)
    worker_init(&worker_pool[i]);
    // Start the thread
    pthread_create(&worker_pool[i].thread, NULL, worker_loop, &worker_pool[i]);
  }
}

// Initialize worker struct
void worker_init(Worker *w) {
  // Create epoll instance
  w->epfd = epoll_create1(0);
  if (w->epfd == -1) {
    perror("epoll_create1");
    exit(EXIT_FAILURE);
  }
  // Create notify pipe
  if (pipe(w->notify_fds) == -1) {
    perror("pipe");
    exit(EXIT_FAILURE);
  }
  // Make the notify pipe's READ end non-blocking
  if (make_socket_nonblocking(w->notify_fds[0]) == -1) {
    perror("make_socket_nonblocking pipe failed");
    exit(EXIT_FAILURE);
  }
  // Register notfy pipe in epoll
  struct epoll_event ev;
  ev.data.fd = w->notify_fds[0];
  ev.events = EPOLLIN;
  epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->notify_fds[0], &ev);

  w->policy = RDY_LIFO; // RDY_LIFO or RDY_FIFO
  w->ready_head = NULL; // head of ready queue
  w->ready_tail = NULL; // tail of ready queue (used for FIFO)
  w->current = NULL;
}

// This is called by the main thread to hand off a descriptor
void cm_dispatch_connection(int client_fd) {
  if (!worker_pool)
    return;

  make_socket_nonblocking(client_fd);

  if (DEBUG) {
    // Declare structures for client data
    struct sockaddr_storage remote_addr; // client address information
    socklen_t sin_size =
        sizeof remote_addr;           // size of client address information
    char remote_ip[INET6_ADDRSTRLEN]; // client ip information

    // Retrieve client info for logging
    if (getpeername(client_fd, (struct sockaddr *)&remote_addr, &sin_size) ==
        -1) {
      perror("getpeername");
      return;
    }
    inet_ntop(remote_addr.ss_family,
              get_in_addr((struct sockaddr *)&remote_addr), remote_ip,
              INET6_ADDRSTRLEN);

    // Log new connection
    log_trace("[+] New client connection (%s:%d) on socket %d", remote_ip,
              ntohs(((struct sockaddr_in *)&remote_addr)->sin_port), client_fd);
  }

  // Round-robin
  int target = next_worker_idx % total_workers;
  next_worker_idx++;

  // Sends a message to a target worker thread via its notify pipe containing
  // the new clients socket. The worker thread registers the socket and polls
  // for writes/reads
  write(worker_pool[target].notify_fds[1], &client_fd, sizeof(int));
}

// The Core Event loop
void *worker_loop(void *arg) {
  Worker *w = (Worker *)arg;
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

      // Notification pipe readable: new accepted client fd(s) handed over by
      // main listener thread
      if (fd == w->notify_fds[0]) {
        /* Drain queued client fds in a batch:
        Here, we read accepted client connections from the notification
        pipe into an array rather than looping read one-by-one, which reduces
        system call overhead if many connections arrive at once. By reading a
        batch of file descriptors in one read() call, you reduce the transition
        between "User Space" and "Kernel Space." When your server is under high
        load (e.g., a burst of 32 connections), this prevents the CPU from doing
        32 small system calls.*/
        int fd_batch[32];
        while (1) {
          // Attempt to read multiple FDs at once
          ssize_t bytes_read = read(fd, fd_batch, sizeof(fd_batch));

          if (bytes_read <= 0) {
            if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
              break; // Pipe is empty
            }
            break; // Error or EOF
          }
          // Calculate how many FDs we actually got
          int num_fds = bytes_read / sizeof(int);

          for (int j = 0; j < num_fds; j++) {
            int client_fd = fd_batch[j];

            // Register client fd in event loop. We register interest for the
            // following events: READBLE (EPOLLIN). WRITETABLE (EPOOLOUT), and
            // DISCONNECTING (EPOLLRDHUP).
            // EPOLLET ensures that we dont perform a busy wait but actually
            struct epoll_event ev = {.data.fd = client_fd,
                                     .events = EPOLLIN | EPOLLOUT | EPOLLRDHUP |
                                               EPOLLET};
            if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
              // Non-fatal error: close this connection and continue
              log_error("epoll_ctl ADD failed. client_fd %d failed to be added "
                        "to the event loop.",
                        client_fd);
              cm_close_connection(w, client_fd);
              continue;
            }

            Coroutine *co =
                coroutine_create(entry, (void *)(uintptr_t)client_fd, w);
            if (co) {
              add_to_ready(w, co);
            } else {
              // Non-fatal error: close this connection and continue
              log_error("Coroutine creation failed for client_fd %d",
                        client_fd);
              cm_close_connection(w, client_fd);
            }
          }

          // If we read fewer than the batch size, the pipe is likely drained
          if (bytes_read < (ssize_t)sizeof(fd_batch)) {
            break;
          }
        }
        continue;
      }

      // fd is disconnecting
      if (event & EPOLLRDHUP) {
        cm_close_connection(w, fd);
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

void cm_close_connection(Worker *w, int client_fd) {
  if (client_fd < 0 || client_fd >= FD_SETSIZE)
    return;

  if (DEBUG) {
    // Declare structures for client data
    struct sockaddr_storage remote_addr; // client address information
    socklen_t sin_size =
        sizeof remote_addr;           // size of client address information
    char remote_ip[INET6_ADDRSTRLEN]; // client ip information

    // Retrieve client info for logging
    if (getpeername(client_fd, (struct sockaddr *)&remote_addr, &sin_size) ==
        0) {
      inet_ntop(remote_addr.ss_family,
                get_in_addr((struct sockaddr *)&remote_addr), remote_ip,
                INET6_ADDRSTRLEN);
      log_trace("[-] Disconnecting client (%s:%d) from socket %d", remote_ip,
                ntohs(((struct sockaddr_in *)&remote_addr)->sin_port),
                client_fd);
    }
  }

  // Networking Cleanup first
  // Remove from epoll BEFORE closing the socket to avoid race conditions
  epoll_ctl(w->epfd, EPOLL_CTL_DEL, client_fd, NULL);
  shutdown(client_fd, SHUT_WR); // Bye!
  close(client_fd);
}
