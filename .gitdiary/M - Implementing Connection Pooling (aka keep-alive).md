### Why Keep-Alive?

**Connection Pooling** (commonly referred to as **Keepalive**) is a technique used to keep connections open between our server and upstream clients after a request completes. 

Without pooling, our server opens and closes a new TCP connection for every single request, which adds significant latency due to the "TCP Handshake" overhead.

**How it Works**

With connection pooling enabled, when our server finishes a request to the upstream. Instead of sending a `FIN` packet to close the connection, it keeps the socket open in a "pool." When the next request for that same upstream arrives, our server grabs the already-open socket and sends the data immediately.

**Why this is critical for our C Server**

If you decide to use this custom C server in production, connection pooling is vital for performance:

- **CPU Savings:** Your C server won't have to constantly run `accept()` and `close()` syscalls for every tiny image or JS file.
- **Latency:** It removes the **3-way TCP handshake** time from every request.
- **Socket Exhaustion:** On high-traffic sites, opening/closing thousands of connections can lead to "TIME_WAIT" socket exhaustion. Pooling prevents this.

---

In your current `entry` function, the coroutine reads one request, sends one response, and terminates. In the modern web, a single page might have 20+ assets (CSS, JS, images). Without Keep-Alive, the browser has to perform a new TCP handshake for every single file, which is slow.

By adding a `while` loop to your `entry` function, the coroutine stays alive and reuses the same socket for multiple requests.

### The "Keep-Alive" Entry Function

```c
#include "../../include/request-handler.h"
#include "./http_parser.c"
#include "./middlewares/middleware_pipeline.c"

#define BUF_SIZE 4096 // 4KB

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

  // Request object
  Request req;

  int keep_alive = 1;
  while (keep_alive) {
    // Allocate and initialize the Request object for this connection
    request_init(&req);

    while (1) {
      // Read bytes from client into buffer
      char buf[BUF_SIZE];
      ssize_t nbytes = recv_async(client_fd, buf, sizeof(buf), 0, 5000, w);

      // 1. Check for disconnect or error
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

      // Parse client bytes from the buffer into Request object
      int parse_state = parse_http(&req, buf, (int)nbytes);

      if (parse_state == 0) // Partially parsed, continue to recv next packets
        continue;

      if (parse_state == 1) { // Successfully parsed, begin middleware pipeline
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

      if (parse_state == -1) { // Failed to parse, send error & close connection
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
```

---

### Why this is a "Game Changer" for our Server:

1. **Resource Reclaim:** Without timeouts, a single "Slowloris" attack (opening 1000 connections and doing nothing) would fill your `fd_table` and stop your server. Now, those dead connections are purged every 5 seconds.
2. **Liveness:** Our server stays snappy because worker threads aren't carrying the "dead weight" of inactive coroutines.
3. **Modern Standards:** This is exactly how Nginx and Apache handle `Keep-Alive` timeouts.
