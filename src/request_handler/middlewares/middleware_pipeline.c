#include "uri_decoder_middleware.c"

// Start Middleware Pipeline
void middleware_pipeline(Request *req, int keep_alive, Worker *w,
                         int client_fd) {
  uri_decoder_middleware(req, keep_alive, w, client_fd);
}
