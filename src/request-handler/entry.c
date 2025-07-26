#include "../../include/request-handler.h"
#include "./include/protocol.h"
#include "../../include/connection.h"
#include <stdio.h>
#include <unistd.h>

#define BUF_SIZE 8

/*
 * This is an exmaple of a coroutine task. Here we can should use our non-blocking functions
 * for read/revc, write/send, sleep, etc. */
void entry(void* arg, Worker* worker)
{
  int fd = *(int*)arg;
  free(arg);

  char buf[BUF_SIZE];
  while (1)
  {
    int nbytes = recv_async(fd, buf, sizeof(buf), 0, worker);
    if (nbytes < 0) perror("recv");

    // We got some good data from a client
    // printf("[*] Received %d bytes from socket %d: %.*s", nbytes, fd, nbytes, buf);
    // if (buf[nbytes-1] != '\n') printf("\n");

    if(parse(fd, buf, nbytes, worker) == -1)
    {
      send_async(fd, "Error: Bad Request\n", 19, 0, worker);
      break;
    }
  }

  close_connection(fd, worker);
}
