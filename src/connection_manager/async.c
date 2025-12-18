#include "../../include/connection_manager.h"
#include "connection_manager.h"
#include <errno.h> // Include for errno and EAGAIN/EWOULDBLOCK
#include <stddef.h>
#include <stdio.h>
#include <sys/socket.h>

/*
 * Attempts to read. If no data is available, it waits until the fd is readable.
 * This function will block the current coroutine until data is available or a
 * non-EAGAIN error occurs.
 */
size_t recv_async(int socket, void *buffer, size_t length, int flags,
                  Worker *worker) {
  while (1) {
    int nbytes = recv(socket, buffer, length, flags);

    // 1. Success or permanent error: Return immediately
    if (nbytes >= 0) {
      return (size_t)nbytes;
    }

    // nbytes is -1. Check errno.
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // 2. Would Block: Yield and wait for I/O readiness
      // The yield function registers the coroutine for the event
      coroutine_yield(worker, socket, WAIT_READ);

      // Execution resumes here after wake_fd() fires (meaning FD is ready).
      // The loop repeats, and we try recv() again.
    } else {
      // 3. Unrecoverable Error (e.g., EBADF, ECONNRESET): Return the error
      // state
      return (size_t)nbytes;
    }
  }
}

/*
 * Attempts to write the entire buffer, yielding if space is not immediately
 * available.
 */
void send_async(int socket, const void *buffer, int length, int flags,
                Worker *worker) {
  int bytes_sent = 0;
  while (bytes_sent < length) {
    // Calculate remaining bytes to send
    const void *current_buffer = (const char *)buffer + bytes_sent;
    int remaining_length = length - bytes_sent;

    int nbytes = send(socket, current_buffer, remaining_length, flags);

    if (nbytes > 0) {
      // Success: update the sent count and continue the loop
      bytes_sent += nbytes;
      continue;
    }

    // nbytes is -1 or 0 (0 is rare/impossible for non-blocking send)
    if (nbytes == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Would Block: Yield and wait for write readiness
        coroutine_yield(worker, socket, WAIT_WRITE);
        // Loop continues and retries send()
      } else {
        // Unrecoverable Error (e.g., connection closed)
        perror("send_async permanent error");
        return; // Abort send operation
      }
    } else {
      // Handle nbytes == 0 case if necessary, usually indicates shutdown/error.
      break;
    }
  }
}
