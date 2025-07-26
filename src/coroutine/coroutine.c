#include "../../include/coroutine.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * Dequeues a coroutine from the top of the ready_queue and swaps to its context.
 * The main_ctx calls this function
 */
void schedule(Worker* worker)
{
  while (worker->ready_queue)
  {
    Coroutine* co = worker->ready_queue;
    worker->ready_queue = co->next;
    worker->current = co;
    swapcontext(&worker->main_ctx, &co->ctx);
  }
}

/*
 * Places current coroutine into the waiting table
 * Yeilds control back to the main_ctx, i.e. the epoll loop, by swapping context
 */
void coroutine_yield(Worker* worker, int fd, WaitType wt)
{
  worker->current->fd = fd;
  worker->current->wait_type = wt;
  worker->fd_table[fd] = worker->current;

  Coroutine* co = worker->current;
  worker->current = NULL;
  swapcontext(&co->ctx, &worker->main_ctx);
}

/*
* Removes coroutine from waiting table and adds it to the front/back of the ready queue.
* Whether to add coroutines to the front or back of the ready queue is a design choice based on tradeoffs. 
*
* Here's why you might add to the front of the queue:
* + Adding them to front lets them complete their work ASAP. Often a coroutines will resume, 
*   do just a little work, and yeild again. Putting it at the front lets it finish its burst 
*   without being preempted by unrelated takss. This works best for low-latency I/O processing.
* However,
* - If one coroutine keeps yeilding and resuming itself, it may starve others, especially if epoll is
*   firing events quickly on just that FD. Newer or more I/O active coroutines may dominate which can
*   make front-insertion sceduling unfair
*
* Here's why you might add to the back of the queue:
* + Coroutines gets a fair turn in the order they became ready. This is what most
*   cooperative multitasking systems do. This prevents busy I/O coroutines from dominating.
* However,
* - A task that would otherwise have taken a short time, would have to wait for its turn. 
*
* We will stick with adding to the front of the queue.
* However, we can refactor in such a way that we can choose the one that makes the most sense 
* each time a coroutine yeilds.
*/
void add_to_ready(Worker* worker, Coroutine* co)
{
  co->next = worker->ready_queue;
  worker->ready_queue = co;
}

/*
* Create a new coroutine to run a specified task asynchronously
* This task may serve as an entry point to run other tasks asynchronously
*/
Coroutine* coroutine_create(void (*fn)(void*, Worker*), void* arg, Worker* worker)
{
  Coroutine* co = malloc(sizeof(Coroutine));
  getcontext(&co->ctx);
  co->ctx.uc_stack.ss_sp = co->stack;
  co->ctx.uc_stack.ss_size = sizeof(co->stack);
  co->ctx.uc_link = &worker->main_ctx;
  makecontext(&co->ctx, (void (*)())fn, 2, arg, worker);
  return co;
}

/*
* Delete a co coroutine and remove it from our data structures
*/
void coroutine_destroy(Worker* worker, Coroutine* co)
{
  // remove coroutine from queue
  Coroutine* prev = NULL;
  Coroutine* curr = worker->ready_queue;
  while (curr) 
  {
    if (curr == co) 
    {
      if (prev) prev->next = curr->next;
      else      worker->ready_queue = curr->next;
      break;
    }
    prev = curr;
    curr = curr->next;
  }

  // free coroutine
  free(co);
}

/*
 * Attempts to read. If no data is available, it waits until the fd is readable */
ssize_t recv_async(int socket, void *buffer, size_t length, int flags, Worker* worker)
{
  int nbytes = recv(socket, buffer, length, flags);

  if (nbytes == -1) coroutine_yield(worker, socket, WAIT_READ);
  else return nbytes;

  return recv(socket, buffer, length, flags);
}

/*
 * Attempts to write. If no space is available, it waits until the fd is writeable */
ssize_t send_async(int socket, const void *buffer, size_t length, int flags, Worker* worker)
{
  int nbytes = send(socket, buffer, length, flags);

  if (nbytes == -1) coroutine_yield(worker, socket, WAIT_WRITE);
  else return nbytes;

  return send(socket, buffer, length, flags);
}

/*
 * This is an exmaple of a coroutine task. Here we can should use our non-blocking functions
 * for read/revc, write/send, sleep, etc. */
void echo_coroutine(void* arg, Worker* worker)
{
  int fd = *(int*)arg;
  free(arg);

  char buf[8];
  while (1)
  {
    int nbytes = recv_async(fd, buf, sizeof(buf), 0, worker);
    if (nbytes < 0) perror("recv");

    // We got some good data from a client
    printf("[*] Received %d bytes from socket %d: %.*s", nbytes, fd, nbytes, buf);
    if (buf[nbytes-1] != '\n') printf("\n");

    send_async(fd, buf, nbytes, 0, worker);
  }
}
