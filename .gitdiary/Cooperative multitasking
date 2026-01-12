### The Big Idea: Why Coroutines?

Traditional multi-threading is "preemptive"—the Operating System manages the CPU and can stop your code at any moment to give another thread a turn. While powerful, this comes with high overhead in memory and context-switching. Coroutines, however, are **cooperative**. A task runs until it reaches a natural waiting point (like waiting for a TCP packet to arrive) and then voluntarily yields control back to the main loop, essentially saying: "I’m waiting on I/O; wake me up when there’s data." This allows us to handle thousands of connections within a single thread.

I’ll show you how I built it all out and explain soon after.

In `includes` / `connection_manager.h`, include the following structures and libraries:

```c
// ...

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

// ...
```

In the `src/connection_manager/` folder, create a file called `coroutine.c`:

```
📂 httplite/
├── 📂 src/
│   ├── 📂 connection_manager/
│   │   └── 📄 coroutine.c
│   └── 📄 main.c
└── ...
```

📄 `src` / `connection_manager` / `coroutine.c`:

```c
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/*
 * Portable trampoline entry. makecontext will call this with one integer-sized
 * argument (we pass a uintptr_t that holds the pointer to TrampolineArgs).
 *
 * This function runs the user fn(arg, worker), then marks the coroutine
 * finished and swaps back to the worker main context.
 */
static void trampoline_start(uintptr_t coptr) {
  Coroutine *co = (Coroutine *)coptr;

  /* 1. Run the user function */
  co->entry_fn(co->arg, co->worker);

  /* 2. Mark finished */
  co->finished = 1;

  /* 3. Return to main_ctx: Swap back to the worker's main event loop
   * (schedule() function) */
  /* We use co->ctx as the source and co->worker->main_ctx as the target */
  swapcontext(&co->ctx, &co->worker->main_ctx);
}

/* ---------- ready queue operations (thread-safe) ---------- */

/*
 * Removes coroutine from waiting table and adds it to the front/back of the
 * ready queue. Whether to add coroutines to the front or back of the ready
 * queue is a design choice based on tradeoffs.
 *
 * Here's why you might add to the front of the queue:
 * + Adding them to front lets them complete their work ASAP. Often a coroutines
 * will resume, do just a little work, and yeild again. Putting it at the front
 * lets it finish its burst without being preempted by unrelated takss. This
 * works best for low-latency I/O processing. However,
 * - If one coroutine keeps yeilding and resuming itself, it may starve others,
 * especially if epoll is firing events quickly on just that FD. Newer or more
 * I/O active coroutines may dominate which can make front-insertion sceduling
 * unfair
 *
 * Here's why you might add to the back of the queue:
 * + Coroutines gets a fair turn in the order they became ready. This is what
 * most cooperative multitasking systems do. This prevents busy I/O coroutines
 * from dominating. However,
 * - A task that would otherwise have taken a short time, would have to wait for
 * its turn.
 *
 * We will stick with adding to the front of the queue.
 * However, we can refactor in such a way that we can choose the one that makes
 * the most sense each time a coroutine yeilds.
 */
void add_to_ready(Worker *worker, Coroutine *co) {
  if (!worker || !co)
    return;

  co->next = NULL;

  if (!worker->ready_head) {
    /* queue empty */
    worker->ready_head = worker->ready_tail = co;
  } else if (worker->policy == RDY_LIFO) {
    /* push front */
    co->next = worker->ready_head;
    worker->ready_head = co;
    /* tail unchanged */
  } else { /* RDY_FIFO */
    /* append at tail */
    worker->ready_tail->next = co;
    worker->ready_tail = co;
  }
}

/* Pop head of ready queue. Caller must hold no locks; function locks
 * internally. Returns popped coroutine or NULL if queue empty.
 */
static Coroutine *pop_ready_head(Worker *worker) {
  Coroutine *co = NULL;

  co = worker->ready_head;
  if (co) {
    worker->ready_head = co->next;
    if (!worker->ready_head)
      worker->ready_tail = NULL;
    co->next = NULL;
  }

  return co;
}

/* ---------- fd table operations (thread-safe) ---------- */

/* Add a coroutine to fd wait slot (1 coroutine per socket) */
void add_to_fd_table(Worker *worker, int fd, Coroutine *co) {
  if (!worker || fd < 0 || fd >= FD_SETSIZE || !co)
    return;

  /* Since it's a 1:1 mapping, we simply assign the pointer.
   * If a previous coroutine was here, it should have been cleaned up by
   * close_connection. */
  worker->fd_table[fd] = co;
}

/* Clear the slot for a specific coroutine */
void remove_from_fd_table(Worker *worker, int fd, Coroutine *co) {
  if (!worker || fd < 0 || fd >= FD_SETSIZE || !co)
    return;

  if (worker->fd_table[fd] == co) {
    worker->fd_table[fd] = NULL;
  }
}

/* Move all coroutines waiting on fd into the ready queue.
 * This is intended to be called by the epoll loop when fd becomes ready.
 */
/* Wake the single coroutine waiting on this fd */
void wake_fd(Worker *worker, int fd) {
  if (!worker || fd < 0 || fd >= FD_SETSIZE)
    return;

  Coroutine *co = worker->fd_table[fd];

  /* We do NOT set the table to NULL here.
   * The connection is still active; we only NULL it when the connection closes.
   */

  if (co != NULL) {
    co->fd = -1;
    co->wait_type = 0;
    add_to_ready(worker, co);
  }
}

/* ---------- core operations ---------- */

/*
 * Places current coroutine into the waiting table and yields to main_ctx.
 *
 * Caller: running coroutine (worker->current must be set)
 */
void coroutine_yield(Worker *worker, int fd, WaitType wt) {
  if (!worker || !worker->current)
    return;

  worker->current->fd = fd;
  worker->current->wait_type = wt;
  add_to_fd_table(worker, fd, worker->current);

  Coroutine *co = worker->current;
  worker->current = NULL;
  swapcontext(&co->ctx, &worker->main_ctx);
}

/*
 * Dequeues coroutines from the ready queue and swaps to their context.
 * When coroutine returns (finished), it will be cleaned up here.
 *
 * Note: schedule() should be executed in a single thread per Worker instance.
 */
void schedule(Worker *worker) {
  if (!worker)
    return;

  while (1) {
    Coroutine *co = pop_ready_head(worker);
    if (!co)
      break;

    worker->current = co;
    swapcontext(&worker->main_ctx, &co->ctx);

    /* swapcontext returns only when coroutine either yielded or finished.
     * Immediately clear 'current' so indicate we are now back in the main
     * context
     */
    worker->current = NULL;

    /* if the coroutine has finished, destroy it now. */
    if (co->finished) {
      coroutine_destroy(worker, co);
    }
    /* otherwise it has been re-enqueued by wake_fd logic */
  }
}

/*
 * Create a new coroutine to run a specified task asynchronously.
 * We allocate a small TrampolineArgs object and pass its pointer as uintptr_t
 * into makecontext. The trampoline frees nothing; coroutine_destroy frees it.
 */
Coroutine *coroutine_create(void (*fn)(void *, Worker *), void *arg,
                            Worker *worker) {
  if (!fn || !worker)
    return NULL;

  Coroutine *co = (Coroutine *)malloc(sizeof(Coroutine));
  if (!co)
    return NULL;

  memset(co, 0, sizeof(*co));
  co->fd = -1;
  co->entry_fn = fn;
  co->arg = arg;
  co->worker = worker;

  if (getcontext(&co->ctx) < 0) {
    free(co);
    return NULL;
  }

  co->ctx.uc_stack.ss_sp = co->stack;
  co->ctx.uc_stack.ss_size = sizeof(co->stack);
  co->ctx.uc_link = &worker->main_ctx;

  /* Pass the co pointer directly to the trampoline start */
  /* Note: makecontext has no failure return; assume it succeeds on supported
   * systems */
  makecontext(&co->ctx, (void (*)(void))trampoline_start, 1, (uintptr_t)co);

  return co;
}

/*
 * Delete a coroutine and remove it from our data structures.
 * Safe to call even if coroutine is not present.
 */
void coroutine_destroy(Worker *worker, Coroutine *co) {
  if (!worker || !co)
    return;
  /* 1) Remove from fd_table if it's waiting on something */
  remove_from_fd_table(worker, co->fd, co);
  /* 2) Free associated memory */
  free(co);
}

```

In the `src/connection_manager/` folder, create another file called `connection_manager.h`:

```
📂 httplite/
├── 📂 src/
│   ├── 📂 connection_manager/
│   │   └── 📄 connection_manager.h
│   └── 📄 main.c
└── ...
```

📄 `src` / `connection_manager` / `connection_manager.h`:

```c
#ifndef CONNECTION_MANAGER_PRIV_H
#define CONNECTION_MANAGER_PRIV_H

#include "../../include/connection_manager.h"

void cm_close_connection(Worker *w, int client_fd);
void coroutine_yield(Worker *worker, int fd, WaitType wt);
void coroutine_destroy(Worker *worker, Coroutine *co);

#endif
```

Then include this file, in `coroutine.c` at the top like so:

```c
#include "connection_manager.h"
```

### 1. The Anatomy of a Coroutine

I used the POSIX `ucontext.h` library to handle the heavy lifting of saving and restoring CPU state. Each coroutine in my system has:

- Its own **Stack** (64KB of private memory).
- A **Context** (registers, instruction pointer).
- A **Status** (Are we waiting to read? Waiting to write? Finished?).

### 2. The Trampoline: Landing Gracefully

One of the trickiest parts of implementation was the "Trampoline." When a coroutine starts, it shouldn't just run a function and disappear; we need a way to return control to the scheduler. I built a `trampoline_start` wrapper that executes the user's logic, marks the coroutine as finished, and then performs a `swapcontext` back to the worker's main event loop. This ensures that when a request is fully handled, the server cleans up the memory and moves seamlessly to the next task.

### 3. The Scheduler: LIFO vs. FIFO

I spent some time weighing the trade-offs of the **Ready Queue** design—the line where coroutines wait for their turn to run:

- **FIFO (First-In-First-Out):** Prioritizes **fairness**. Every connection gets a turn in the order it became ready, preventing any single task from being starved.
- **LIFO (Last-In-First-Out):** Prioritizes **performance**. By running the most recent task immediately, you increase "cache locality"—the data that task needs is likely still in the CPU's high-speed cache.

I implemented a `ReadyPolicy` toggle so I can experiment with both as the server grows.

```c
if (worker->policy == RDY_LIFO) {
    // Push to front: Priority for low-latency bursts
    co->next = worker->ready_head;
    worker->ready_head = co;
} else {
    // Append to tail: Fairness for all connections
    worker->ready_tail->next = co;
    worker->ready_tail = co;
}
```

### 4. The Yield and Wake Cycle

The core of this architecture is `coroutine_yield`. When a socket isn't ready, the coroutine "parks" itself in an `fd_table`. The worker then runs the `schedule()` function to find other tasks that are ready to work. Once the `epoll` loop (which we’ll implement next) detects data on that specific file descriptor, it calls `wake_fd`, moving the coroutine back into the "Ready" queue to resume exactly where it left off.

### Lessons Learned

- **State Management is Hard:** In C, you have to be incredibly careful with pointers. If a coroutine is destroyed while it's still in the `fd_table`, you're looking at a segmentation fault.
- **Stack Size:** 64KB is plenty for an HTTP request, but it's much smaller than a standard 8MB thread stack. This is how we'll support 10,000+ connections on a modest machine.
