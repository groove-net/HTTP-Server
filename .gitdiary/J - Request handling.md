### The Coroutine Lifecycle

This entry brings everything together. The `entry` function is the "brain" of each connection and because each connection is mapped to its own coroutine, this function can be written in a straightforward, linear style, even though the underlying I/O is completely asynchronous.

In the `src` / `request_handler` / `http_utils.c` file let’s build out the `entry` function:

```c
#include "../../include/request-handler.h"
#include "http_parser.c"
#include "http_utils.c"

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
      break;
    }

    // Parse client bytes from the buffer into Request object
    int parse_state = parse_http(&req, buf, (int)nbytes);

    if (parse_state == 0) // Partially parsed, continue to recv next packets
      continue;

    if (parse_state == 1) { // Successfully parsed, begin middleware pipeline
      // TODO: Middleware pipeline starts here...
      request_cleanup(&req);
      break;
    }

    if (parse_state == -1) { // Failed to parse, send error and close connection
      send_error(client_fd, 400, NULL, 0, w);
      request_cleanup(&req);
      break;
    }
  }

  // Hardware close
  cm_close_connection(w, client_fd);
}
```

### 1. State Ownership

In C, managing state across asynchronous calls is usually a nightmare of callbacks and pointers. Here, the coroutine's private stack solves that problem. We allocate the `Request` object directly on the stack (or manage its lifecycle within the function). This ensures that every connection has its own isolated memory space for headers, URI strings, and parsing state without needing complex global synchronization.

### 2. The Asynchronous Read Loop

The heart of the handler is the `while(1)` loop that pulls data from the client:

```c
char buf[BUF_SIZE];
ssize_t nbytes = recv_async(client_fd, buf, sizeof(buf), 0, 5000, w);
```

By using `recv_async`, we are telling the worker: "If there's no data, park this coroutine and let someone else use the CPU." When data arrives or the 5000ms timeout hits, the scheduler resumes this specific function right where it left off.

### 3. Handling the Byte Stream

Because TCP is a stream protocol (as we discussed in the foundations entry), we cannot assume an HTTP request will arrive in a single `recv` call. Our handler supports three distinct parsing states:

- **Partial (0):** The headers aren't complete yet (we're still waiting for that final `\r\n\r\n`). We `continue` the loop to fetch more bytes.
- **Success (1):** The request is fully formed. We are ready to hand this off to the middleware pipeline (routing, static file serving, etc.).
- **Failure (-1):** The client sent malformed data. We immediately dispatch a `400 Bad Request` and terminate the connection.

### 4. The "Hardware" Close

One of the core principles of this server is that a failure in a single request should never crash the process. Whether a timeout occurs, a client disconnects, or a parser error is triggered, the coroutine cleans up its own `Request` object and triggers `cm_close_connection`. This ensures that file descriptors and memory are returned to the system promptly, preventing leaks.
