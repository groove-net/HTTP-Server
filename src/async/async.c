#include "../../include/async.h"
#include <stddef.h>
#include <stdio.h>
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
void send_async(int socket, const void *buffer, int length, int flags, Worker* worker)
{
  int nbytes = send(socket, buffer, length, flags);
  printf("nbytes is %d & total length is %d\n", nbytes, length);
  if (nbytes == -1) nbytes = 0;
  while (nbytes < length)
  {
    coroutine_yield(worker, socket, WAIT_WRITE);
    nbytes += send(socket, buffer, length, flags);
    printf("nbytes is %d & total length is %d\n", nbytes, length);
  }
}

