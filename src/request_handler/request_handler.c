#include "../../include/connection_manager.h"
#include "./middlewares/middleware_pipeline.c"
#include "protocol.c"

#define BUF_SIZE 4096

/*
 * This is the entry point for every coroutine/connection (each connection maps
 * to a coroutine). This entry function is the coroutine's lifetime boundary. It
 * should own the state. This request struct must be allocated once and managed
 * by the coroutine. We must ensure we pass the Request* to parse. From this
 * point forward, we should use our async methods for read/revc, write/send,
 * sleep, etc. From this point forward, fatal errors should not quit the server
 * program but close the connection.
 */
void entry(void *arg, Worker *w) {
  // Cast the pointer back to an integer value
  int client_fd = (int)(uintptr_t)arg;

  Request req;

  int keep_alive = 1;
  while (keep_alive) {
    // Allocate and initialize the Request object once per full request
    request_init(&req);

    while (1) {
      char buf[BUF_SIZE]; // Read into a temporary buffer

      // Get packets from client
      ssize_t nbytes = recv_async(client_fd, buf, sizeof(buf), 0, 5000, w);

      if (nbytes <= 0) {
        /**
         * Pemanent error
         * If nbytes == 0, then the client has disconnected.
         * If nbytes < 0, then an unrecoverable error (e.g., EBADF, ECONNRESET,
         * ETIMEDOUT) has occured. Consider handling/logging these errors.
         * Either way, must close connection.
         **/
        request_cleanup(&req);
        keep_alive = 0;
        break;
      }

      // Parse client's packets into Request object
      int parse_state = parse_http(&req, buf, (int)nbytes);

      if (parse_state == 0) // Partial
        continue;

      if (parse_state == 1) { // Success
        // Check if the client wants to keep the connection open
        // By default, HTTP/1.1 is keep-alive. We check for "close"
        const char *conn_val = get_header(&req, "Connection");
        if (conn_val != NULL && strcasecmp(conn_val, "close") == 0) {
          keep_alive = 0;
        }
        // Start middleware pipeline
        middleware_pipeline(&req, keep_alive, w, client_fd);
        request_cleanup(&req);
        break;
      }

      if (parse_state == -1) { // Request formet error, Close connection
        send_error(client_fd, 400, NULL, 0, w);
        request_cleanup(&req);
        keep_alive = 0;
        break;
      }
    }
  }

  // Hardware close
  cm_close_connection(w, client_fd);
}
