#include "../../include/async.h"
#include <sys/socket.h>

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

