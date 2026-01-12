## Bridging the Gap—Async I/O and the Illusion of Blocking

In the previous logs, I built the **Coroutine Runtime** and the **Multi-threaded Epoll Worker Pool**. Today, I’m implementing the most critical abstraction in the entire project: the **Async I/O layer**.

In this entry, we are diving deep into the implementation of our core I/O primitives: `recv_async`, `send_async`, and `sendfile_async`.  This is where the magic happens. To the developer writing the HTTP handler, these functions look and feel like standard, synchronous blocking calls. Under the hood, however, they are performing a complex dance of yielding and resuming that allows a single thread to juggle thousands of concurrent requests. With this, we can handle high-throughput traffic without the overhead of traditional thread-based blocking.

---

### 1. Setup

In the `src/connection_manager/` folder, create a file called `async.c`:

```
📂 httplite/
├── 📂 src/
│   ├── 📂 connection_manager/
│   │   └── 📄 async.c
│   └── 📄 main.c
└── ...
```

### 2. Includes

In the create a file called `async.c`, paste in the following includes.

```c
#define _GNU_SOURCE // This tells the compiler to allow Linux-specific functions
#include "../../include/connection_manager.h"
#include "connection_manager.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
```

### 3. `recv_async`

```c
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
```

The `recv_async` function is designed to handle the inherent unpredictability of network latency by introducing a mandatory timeout mechanism. Using `clock_gettime(CLOCK_MONOTONIC, &start)`, we establish a stable time reference that is unaffected by system clock jumps. The function operates within a persistent `while(1)` loop, attempting a standard `recv` call. If data is present, it returns immediately.

However, when the socket returns `EAGAIN` or `EWOULDBLOCK`, the function enters its logic for cooperative suspension. Before yielding control back to the worker's scheduler, it calculates the elapsed time. If the duration exceeds the defined `timeout_ms`, the function manually injects `ETIMEDOUT` into `errno` and returns a failure state. This prevents "zombie" connections from holding onto coroutine stacks indefinitely. If time remains, `coroutine_yield` is invoked, parking the coroutine in the worker's `fd_table` until `epoll` signals that the descriptor is once again readable.

### 4. `send_async`

```c
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
```

Writing to a non-blocking socket requires a sophisticated approach to partial success. When the kernel's outgoing socket buffer is near capacity, a `send` call may only accept a fraction of the provided data. `send_async` manages this by maintaining a `bytes_sent` counter, which acts as a cursor for the transmission.

The technical complexity lies in the pointer arithmetic used to resume the operation. By casting the `void *` buffer to a `const char *`, we can precisely calculate the `current_buffer` address for each iteration: `(const char *)buffer + bytes_sent`. If the kernel returns `EAGAIN`, the coroutine yields with a `WAIT_WRITE` status. Upon being woken by an `EPOLLOUT` event, the coroutine resumes exactly where it left off, retrying the `send` with the remaining length. This loop ensures that the entire payload is eventually committed to the network stack without blocking the worker thread or losing data integrity.

### 4. `sendfile_async`

```c
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

```

For serving static content, we bypass the standard user-space buffering entirely via `sendfile_async`. Traditional file transfers involve multiple context switches and data copies between the kernel and user space. By utilizing the Linux-specific `sendfile` system call, we instruct the kernel to move data directly from the file system cache to the network interface card (NIC) buffer.

To maximize the efficiency of this "Zero-Copy" path, we use `posix_fadvise(file_fd, 0, file_size, POSIX_FADV_SEQUENTIAL)`. This hint allows the kernel's virtual memory manager to optimize its read-ahead algorithms, pre-loading file data into the page cache before the `sendfile` loop demands it. Even in this optimized state, the operation must remain cooperative; if the socket's write buffer saturates, `sendfile` returns `EAGAIN`. Our implementation handles this by yielding to the scheduler, then resuming the transfer using the `offset` parameter, which the kernel automatically updates to reflect the number of bytes successfully "plumbed" into the socket.

---

### What’s Next?

We now have a high-performance I/O layer capable of zero-copy file transfers and timed-out reads. The foundation is complete. In the next entry, we shift from "how to move bytes" to "what those bytes mean" as we begin building our **HTTP/1.1 Request Parser**.
