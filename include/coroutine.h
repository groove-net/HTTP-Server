#pragma once

#include <ucontext.h>
#include <stdlib.h>

#define STACK_SIZE (64 * 1024)

typedef enum { WAIT_READ, WAIT_WRITE } WaitType;

typedef struct Coroutine {
    ucontext_t ctx;
    char stack[STACK_SIZE];
    int fd;
    WaitType wait_type;
    struct Coroutine* next;
} Coroutine;

typedef struct Worker {
  int epfd;
  int notify_fds[2];               // pipe for main-thread wakeup
  Coroutine* ready_queue;          // queue of ready coroutines (however it uses LIFO scheduling with a stack data structure)
  Coroutine* fd_table[FD_SETSIZE]; // table of waiting coroutines (fd → coroutine)
  ucontext_t main_ctx;             // main context i.e. the epoll loop
  Coroutine* current;              // coroutine currently running. If main_ctx is running, current = NULL
  pthread_t thread;
  int id;
  // A coroutine typically cycles between ready_queue, fd_table, and current
} Worker;

void schedule(Worker*);
void add_to_ready(Worker*, Coroutine*);
Coroutine* coroutine_create(void (*fn)(void*, Worker*), void*, Worker*);
void coroutine_destroy(Worker*, Coroutine*);
void echo_coroutine(void*, Worker*);
