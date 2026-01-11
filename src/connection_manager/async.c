#define _GNU_SOURCE // This tells the compiler to allow Linux-specific functions
#include "../../include/connection_manager.h"
#include "connection_manager.h"
#include <errno.h>
#include <errno.h> // Include for errno and EAGAIN/EWOULDBLOCK
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/*
 * Attempts to read. If no data is available, it waits until the fd is readable.
 * This function will block the current coroutine until data is available or a
 * non-EAGAIN error occurs.
 */
size_t recv_async(int socket, void *buffer, size_t length, int flags,
                  int timeout_ms, Worker *worker) {
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);

  while (1) {
    int nbytes = recv(socket, buffer, length, flags);

    // 1. Success or permanent error: Return immediately
    if (nbytes >= 0) {
      return (size_t)nbytes;
    }

    // nbytes is -1. Check errno.
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // 2. Would Block: Yield and wait for I/O readiness
      // Check if we have timed out before yielding
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      double elapsed = (now.tv_sec - start.tv_sec) * 1000.0 +
                       (now.tv_nsec - start.tv_nsec) / 1000000.0;

      if (elapsed >= timeout_ms) {
        errno = ETIMEDOUT;
        return -1;
      }

      // Still have time? Yield and wait for epoll
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

/*
 * Send files to sockets efficiently
 */
void sendfile_async(int socket, const char *path, off_t file_size,
                    Worker *worker) {
  int file_fd = open(path, O_RDONLY);
  if (file_fd < 0)
    return;

  // Tell the kernel: "I'm going to read this whole thing sequentially"
  posix_fadvise(file_fd, 0, file_size, POSIX_FADV_SEQUENTIAL);

  off_t offset = 0;
  // Linux sendfile handles the loop internally, but for very large files,
  // we loop to ensure the whole file is sent if the socket buffer fills up.
  while (offset < file_size) {
    ssize_t sent = sendfile(socket, file_fd, &offset, file_size - offset);
    if (sent <= 0) {
      if (errno == EINTR)
        continue; // Just try again
      if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // If the socket buffer is full, yield and try again
        coroutine_yield(worker, socket, WAIT_WRITE);
        continue;
      }
      break; // Real error
    }
  }
  close(file_fd);
}
