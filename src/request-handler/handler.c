#include <sys/socket.h>
#include "./include/handler.h"
#include "../../include/async.h"
#include "../../include/connection.h"

void handler (int fd, char* req, int nbytes, Worker* worker)
{
  char tmp;
  for (int i = 0, j = nbytes-2; i < (nbytes-1)/2; i++, j--)
  {
    tmp = req[i];
    req[i] = req[j];
    req[j] = tmp;
  }

  send_async(fd, req, nbytes, 0, worker);
  close_connection(fd, worker);
}
