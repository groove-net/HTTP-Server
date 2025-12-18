#include "../../include/connection_manager.h"
#include "request_handler.h"

void handler(Request *req, Worker *w, int client_fd) {
  (void)req;
  send_async(client_fd, "Parsing complete\n", 17, 0, w);
}
