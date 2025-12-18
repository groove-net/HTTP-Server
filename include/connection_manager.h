#ifndef CONNECTION_MANAGER_PUB_H
#define CONNECTION_MANAGER_PUB_H

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/select.h> // for FD_SETSIZE
#include <ucontext.h>

#define STACK_SIZE (64 * 1024)

typedef enum WaitType { WAIT_READ, WAIT_WRITE } WaitType;
typedef enum ReadyPolicy { RDY_LIFO, RDY_FIFO } ReadyPolicy;

// Forward declarations
struct Worker;
typedef struct Worker Worker;

// Define Coroutine
typedef struct Coroutine {
  ucontext_t ctx;
  char stack[STACK_SIZE];
  int fd;
  WaitType wait_type;
  struct Coroutine *next; // ready queue or fd list
  int finished;           // 1 if the coroutine has returned

  // Direct storage for entry logic
  void (*entry_fn)(void *, struct Worker *);
  void *arg;
  struct Worker *worker;
} Coroutine;

// Define Worker
struct Worker {
  int epfd;
  int notify_fds[2];               // pipe for main-thread wakeup
  Coroutine *ready_head;           // head of ready queue
  Coroutine *ready_tail;           // tail of ready queue (used for FIFO)
  ReadyPolicy policy;              // RDY_LIFO or RDY_FIFO
  Coroutine *fd_table[FD_SETSIZE]; // list of waiting coroutines mapped
                                   // to the fd they are waiting on.
                                   // fd_table[fd] returns the SINGLE coroutine
                                   // waiting on that fd
  ucontext_t main_ctx;             // main context i.e. the epoll loop
  Coroutine *current; // coroutine currently running. if main_ctx is running,
                      // current = NULL
  pthread_t thread;
  // A coroutine typically cycles between ready_queue, fd_table, and current
};

// High-level Pool Management
void cm_init_thread_pool(int num_workers);
void cm_dispatch_connection(int client_fd);
void cm_close_connection(Worker *w, int client_fd);

// Async functions for coroutines
size_t recv_async(int socket, void *buffer, size_t length, int flags,
                  Worker *worker);
void send_async(int socket, const void *buffer, int length, int flags,
                Worker *worker);

#endif
