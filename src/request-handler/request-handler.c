#include <string.h>
#include <sys/socket.h>

void handler (char * req, int clientfd)
{
  int len = strlen(req);
  char tmp;
  for (int i = 0, j = len-1; i < len/2; i++, j--)
  {
    tmp = req[i];
    req[i] = req[j];
    req[j] = tmp;
  }

  send(clientfd, req, len, 0);
}
