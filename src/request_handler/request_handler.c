#include "../../include/connection_manager.h"
#include "handler.c"
#include "protocol.c"

#define BUF_SIZE 256

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
  int client_fd = *(int *)arg;
  free(arg);

  // Allocate and initialize the Request object for this connection
  Request req;
  request_init(&req);

  // Parsing and constructing on the Request object
  char buf[BUF_SIZE];
  while (1) {
    // Get packets from client
    int nbytes = recv_async(client_fd, buf, sizeof(buf), 0, w);

    // 1. Check for disconnect or error
    if (nbytes <= 0) {
      /**
       * If nbytes == 0, then the client has disconnected.
       * If nbytes < 0, then an unrecoverable error (e.g., EBADF, ECONNRESET)
       * has occured. Consider handling these errors.
       * Either way, goto cleanup.
       **/
      goto cleanup;
    }

    // Parse client's packaets into Request object (only if nbytes > 0)
    int parse_state = parse_http(&req, buf, nbytes);

    if (parse_state == -1) { // Bad Request, send error and close connection
      send_async(client_fd, "Error: Bad Request\n", 19, 0, w);
      goto cleanup;
    }
    if (parse_state == 0) { // Partially parsed, continue to recv next packets
      continue;
    }
    if (parse_state == 1) { // Completely parsed, break recieve loop
      break;
    }
  }

  // Request handling
  handler(&req, w, client_fd);

cleanup:
  // Close the connection and cleanup the request struct
  cm_close_connection(w, client_fd);
  request_cleanup(&req);
  return;
}
