#include "../../include/async.h"
#include "../../include/connection_manager.h"
#include "request_handler.h"
#include <stdio.h>

void handler(int fd, Request *req, Worker *worker) {

  printf("\n--- PARSING COMPLETE (State: %s) ---\n",
         (req->state == DONE ? "DONE" : "ERROR"));
  if (req->state != DONE)
    return;

  printf("Method: %s\n", req->method);
  printf("URI:    %s\n", req->uri);
  printf("Version: %s\n", req->version);

  printf("Headers (%d):\n", req->header_count);
  for (int i = 0; i < req->header_count; i++) {
    printf("  > %s: %s\n", req->headers[i].key, req->headers[i].value);
  }
  printf("--------------------------------------\n");

  send_async(fd, "Parsing complete\n", 17, 0, worker);
  close_connection(fd, worker);
}
