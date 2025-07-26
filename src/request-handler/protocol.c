#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <stddef.h>
#include "./include/protocol.h"
#include "./include/handler.h"

typedef enum { NEW, INPROGRESS } RequestState;

typedef struct Request
{
  char* buf;
  RequestState state;
  ssize_t buf_size;
  ssize_t buf_len;
} Request;

Request* req = NULL;

void request_create()
{
  req = malloc(sizeof(Request));
  req->buf_size = 4096; // Start with 4KB
  req->buf = malloc(req->buf_size);
  req->buf_len = 0;
  req->state = NEW;
}

void request_grow()
{
  req->buf_size *= 2;
  req->buf = realloc(req->buf, req->buf_size);
}

int parse(int fd, const char* buf, int nbytes, Worker* worker)
{
  if (req == NULL) request_create();
  if (req->buf_len + nbytes > req->buf_size) request_grow();

  for (int i = 0; i < nbytes; i++)
  {
    switch (req->state)
    {
      case NEW:
        if (buf[i] == '\n') continue;
        if (buf[i] != '`') return -1; // Bad request
        req->state = INPROGRESS;
        break;

      case INPROGRESS:
        if (buf[i] == '`')
        {
          if (req->buf_len == 0) return -1; // Empty request
          req->buf[req->buf_len] = '\n';
          req->buf_len++;
          handler(fd, req->buf, req->buf_len, worker);

          // reset Request struct
          free(req);
          request_create();
          continue;
        }
          
        req->buf[req->buf_len] = buf[i];
        req->buf_len++; 
        break;

      default:
        break; 
    }
  }

  return 1;
}
