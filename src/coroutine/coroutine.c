#include "../../include/coroutine.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Internal trampoline args struct - passed through makecontext as a
 * pointer-sized arg */
typedef struct TrampolineArgs {
  void (*fn)(void *, Worker *);
  void *arg;
  Worker *worker;
} TrampolineArgs;

/*
 * Portable trampoline entry. makecontext will call this with one integer-sized
 * argument (we pass a uintptr_t that holds the pointer to TrampolineArgs).
 *
 * This function runs the user fn(arg, worker), then marks the coroutine
 * finished and swaps back to the worker main context.
 */
static void trampoline_start(uintptr_t tptr) {
  TrampolineArgs *t = (TrampolineArgs *)(uintptr_t)tptr;
  /* Run user function */
  t->fn(t->arg, t->worker);

  /* Mark finished and return to main_ctx */
  if (t->worker->current) {
    t->worker->current->finished = 1;
    /* swap back to main context; trampoline args will be freed by
     * coroutine_destroy */
    swapcontext(&t->worker->current->ctx, &t->worker->main_ctx);
  } else {
    /* extremely unlikely: no current coroutine set */
    swapcontext(&t->worker->main_ctx, &t->worker->main_ctx);
  }
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

  pthread_mutex_lock(&worker->ready_mutex);

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

  pthread_mutex_unlock(&worker->ready_mutex);
}

/* Pop head of ready queue. Caller must hold no locks; function locks
 * internally. Returns popped coroutine or NULL if queue empty.
 */
static Coroutine *pop_ready_head(Worker *worker) {
  Coroutine *co = NULL;
  pthread_mutex_lock(&worker->ready_mutex);
  co = worker->ready_head;
  if (co) {
    worker->ready_head = co->next;
    if (!worker->ready_head)
      worker->ready_tail = NULL;
    co->next = NULL;
  }
  pthread_mutex_unlock(&worker->ready_mutex);
  return co;
}

/* ---------- fd table operations (thread-safe) ---------- */

/* Add a coroutine to fd wait list (multiple waiters allowed) */
void add_to_fd_table(Worker *worker, int fd, Coroutine *co) {
  if (!worker || fd < 0 || fd >= FD_SETSIZE || !co)
    return;

  FdNode *node = (FdNode *)malloc(sizeof(FdNode));
  if (!node) {
    /* Allocation failure: abort yield path (best-effort) */
    perror("add_to_fd_table: malloc failed");
    return;
  }
  node->co = co;

  pthread_mutex_lock(&worker->fd_mutex);
  node->next = worker->fd_table[fd];
  worker->fd_table[fd] = node;
  pthread_mutex_unlock(&worker->fd_mutex);
}

/* Remove all nodes from fd_table[fd] that refer to 'co' */
void remove_from_fd_table(Worker *worker, int fd, Coroutine *co) {
  if (!worker || fd < 0 || fd >= FD_SETSIZE || !co)
    return;

  pthread_mutex_lock(&worker->fd_mutex);
  FdNode *prev = NULL;
  FdNode *curr = worker->fd_table[fd];
  while (curr) {
    FdNode *next = curr->next;
    if (curr->co == co) {
      if (prev)
        prev->next = next;
      else
        worker->fd_table[fd] = next;
      free(curr);
      /* continue scanning — remove all occurrences */
      curr = next;
      continue;
    }
    prev = curr;
    curr = next;
  }
  pthread_mutex_unlock(&worker->fd_mutex);
}

/* Move all coroutines waiting on fd into the ready queue.
 * This is intended to be called by the epoll loop when fd becomes ready.
 */
void wake_fd(Worker *worker, int fd) {
  if (!worker || fd < 0 || fd >= FD_SETSIZE)
    return;

  /* Detach the entire list under lock, then push each to ready queue */
  pthread_mutex_lock(&worker->fd_mutex);
  FdNode *head = worker->fd_table[fd];
  worker->fd_table[fd] = NULL;
  pthread_mutex_unlock(&worker->fd_mutex);

  for (FdNode *n = head; n != NULL;) {
    FdNode *next = n->next;
    Coroutine *co = n->co;
    /* clear coroutine's fd record (optional) */
    co->fd = -1;
    co->wait_type = 0;
    /* add coroutine to ready queue (thread-safe) */
    add_to_ready(worker, co);
    /* free node */
    free(n);
    n = next;
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

    /* After swapcontext returns, coroutine either yielded or finished.
     * If it finished, destroy it now.
     */
    if (co->finished) {
      coroutine_destroy(worker, co);
    }
    /* otherwise it has been re-enqueued by yield/wake logic */
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
  co->finished = 0;
  co->trampoline_args = NULL;

  if (getcontext(&co->ctx) < 0) {
    perror("coroutine_create: getcontext failed");
    free(co);
    return NULL;
  }

  co->ctx.uc_stack.ss_sp = co->stack;
  co->ctx.uc_stack.ss_size = sizeof(co->stack);
  co->ctx.uc_link = &worker->main_ctx;

  /* Prepare trampoline args */
  TrampolineArgs *t = (TrampolineArgs *)malloc(sizeof(*t));
  if (!t) {
    perror("coroutine_create: trampoline alloc failed");
    free(co);
    return NULL;
  }
  t->fn = fn;
  t->arg = arg;
  t->worker = worker;
  co->trampoline_args = t;

  /* makecontext: pass pointer as a single uintptr_t argument (portable) */
  /* Note: makecontext has no failure return; assume it succeeds on supported
   * systems */
  makecontext(&co->ctx, (void (*)(void))trampoline_start, 1, (uintptr_t)t);

  return co;
}

/*
 * Delete a coroutine and remove it from our data structures.
 * Safe to call even if coroutine is not present.
 */
void coroutine_destroy(Worker *worker, Coroutine *co) {
  if (!worker || !co)
    return;

  /* If co is the currently running coroutine, don't free it here.
   * schedule() will call coroutine_destroy only after the coroutine returns
   * control.
   */

  /* 1) Remove from ready queue if present */
  /* Here, we remove only the first occurrence of co from the ready queue. In
   * normal, correct usage a coroutine should only be enqueued once at a time —
   * but bugs or races might cause duplicates. To be defensive, consider
   * removing all occurrences */
  pthread_mutex_lock(&worker->ready_mutex);
  Coroutine *prev = NULL;
  Coroutine *curr = worker->ready_head;
  while (curr) {
    if (curr == co) {
      if (prev)
        prev->next = curr->next;
      else
        worker->ready_head = curr->next;
      if (worker->ready_tail == co)
        worker->ready_tail = prev;
      break;
    }
    prev = curr;
    curr = curr->next;
  }
  pthread_mutex_unlock(&worker->ready_mutex);

  /* 2) Remove from all fd_table entries (ensure all occurrences removed) */
  for (int fd = 0; fd < FD_SETSIZE; ++fd) {
    remove_from_fd_table(worker, fd, co);
  }

  /* 3) Free trampoline args if any */
  if (co->trampoline_args) {
    free(co->trampoline_args);
    co->trampoline_args = NULL;
  }

  /* 4) free coroutine itself */
  free(co);
}
