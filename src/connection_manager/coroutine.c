#include "../../include/connection_manager.h"
#include "connection_manager.h"
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
