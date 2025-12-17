#pragma once

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/select.h> // for FD_SETSIZE
#include <ucontext.h>

#define STACK_SIZE (64 * 1024)

typedef enum WaitType { WAIT_READ, WAIT_WRITE } WaitType;
typedef enum ReadyPolicy { RDY_LIFO, RDY_FIFO } ReadyPolicy;

typedef struct Coroutine {
  ucontext_t ctx;
  char stack[STACK_SIZE];
  int fd;
  WaitType wait_type;
  struct Coroutine *next; // ready queue or fd list
  int finished;           // 1 if the coroutine has returned

  /* pointer to trampoline args (opaque blob). Freed when coroutine is destroyed
   */
  void *trampoline_args;
} Coroutine;

/* Simple singly-linked list node for fd waiters */
typedef struct FdNode {
  Coroutine *co;
  struct FdNode *next;
} FdNode;

typedef struct Worker {
  int id;
  int epfd;
  int notify_fds[2];            // pipe for main-thread wakeup
  Coroutine *ready_head;        // head of ready queue
  Coroutine *ready_tail;        // tail of ready queue (used for FIFO)
  ReadyPolicy policy;           // RDY_LIFO or RDY_FIFO
  FdNode *fd_table[FD_SETSIZE]; // list of waiting coroutines per FD
  ucontext_t main_ctx;          // main context i.e. the epoll loop
  Coroutine *current; // coroutine currently running. if main_ctx is running,
                      // current = NULL
  pthread_t thread;

  pthread_mutex_t ready_mutex;
  pthread_mutex_t fd_mutex;
  // A coroutine typically cycles between ready_queue, fd_table, and current
} Worker;

/* core API (kept same names) */
void schedule(Worker *);
void coroutine_yield(Worker *, int, WaitType);
void add_to_ready(Worker *, Coroutine *);
Coroutine *coroutine_create(void (*fn)(void *, Worker *), void *, Worker *);
void coroutine_destroy(Worker *, Coroutine *);

/* Helper useful for the epoll loop:
 *   - wake_fd(worker, fd) : move all coroutines waiting on fd to ready queue
 * You can call wake_fd() from your epoll loop when an fd becomes
 * readable/writable.
 */
void wake_fd(Worker *worker, int fd);
