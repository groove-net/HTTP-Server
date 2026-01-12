## Scaling with Multi-Threaded Epoll and Edge-Triggered Events

In the previous entry, I built the coroutine runtime. Today, I’m integrating that runtime into a high-performance, multi-threaded event loop using Linux `epoll`. We are moving toward a **Worker Pool** architecture, where a main thread distributes connections to dedicated worker threads, each managing its own independent event loop.

---

Add all the following to `connection_manager.c`:

- Includes
    
    ```c
    #include "coroutine.c"
    #include <errno.h>
    #include <sys/epoll.h>
    ```
    
- Static globals
    
    ```c
    static Worker *worker_pool = NULL;
    static int total_workers = 0;
    static int next_worker_idx = 0;
    ```
    
- Functions
    
    ```c
    // Initialize worker struct
    void worker_init(Worker *w) {
      // Create epoll instance
      w->epfd = epoll_create1(0);
      if (w->epfd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
      }
      // Create notify pipe
      if (pipe(w->notify_fds) == -1) {
        perror("pipe");
        exit(EXIT_FAILURE);
      }
      // Make the notify pipe's READ end non-blocking
      if (make_socket_nonblocking(w->notify_fds[0]) == -1) {
        perror("make_socket_nonblocking pipe failed");
        exit(EXIT_FAILURE);
      }
      // Register notfy pipe in epoll
      struct epoll_event ev;
      ev.data.fd = w->notify_fds[0];
      ev.events = EPOLLIN;
      epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->notify_fds[0], &ev);
    
      w->ready_head = NULL; // head of ready queue
      w->ready_tail = NULL; // tail of ready queue (used for FIFO)
      w->current = NULL;
    }
    ```
    
    ---
    
    ```c
    void cm_init_thread_pool(int num_workers) {
      total_workers = num_workers;
      worker_pool = malloc(sizeof(Worker) * num_workers);
    
      for (int i = 0; i < num_workers; i++) {
        // Initialize worker internals (epoll, pipes, mutexes)
        worker_init(&worker_pool[i]);
        // Start the thread
        pthread_create(&worker_pool[i].thread, NULL, worker_loop, &worker_pool[i]);
      }
    }
    ```
    
    ---
    
    ```c
    #define MAX_EVENTS 64 // Max number of events returned by epoll
    
    // The Core Event loop
    void *worker_loop(void *arg) {
      Worker *w = (Worker *)arg;
      struct epoll_event events[MAX_EVENTS];
      while (1) {
        // Pause thread until an event occurs on a socket
        int nfds = epoll_wait(w->epfd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
          perror("epoll_wait");
          exit(EXIT_FAILURE);
        }
    
        // Once event occurs on one or more sockets, loop through the returned fds
        for (int i = 0; i < nfds; i++) {
          int fd = events[i].data.fd;        // get fd
          uint32_t event = events[i].events; // get event
    
          // Notification pipe readable: new accepted client fd(s) handed over by
          // main listener thread
          if (fd == w->notify_fds[0]) {
            /* Drain queued client fds in a batch:
            Here, we read accepted client connections from the notification
            pipe into an array rather than looping read one-by-one, which reduces
            system call overhead if many connections arrive at once. By reading a
            batch of file descriptors in one read() call, you reduce the transition
            between "User Space" and "Kernel Space." When your server is under high
            load (e.g., a burst of 32 connections), this prevents the CPU from doing
            32 small system calls.*/
            int fd_batch[32];
            while (1) {
              // Attempt to read multiple FDs at once
              ssize_t bytes_read = read(fd, fd_batch, sizeof(fd_batch));
    
              if (bytes_read <= 0) {
                if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                  break; // Pipe is empty
                }
                break; // Error or EOF
              }
              // Calculate how many FDs we actually got
              int num_fds = bytes_read / sizeof(int);
    
              for (int j = 0; j < num_fds; j++) {
                int client_fd = fd_batch[j];
    
                // Register client fd in event loop. We register interest for the
                // following events: READBLE (EPOLLIN). WRITETABLE (EPOOLOUT), and
                // DISCONNECTING (EPOLLRDHUP).
                // EPOLLET ensures that we dont perform a busy wait but actually
                struct epoll_event ev = {.data.fd = client_fd,
                                         .events = EPOLLIN | EPOLLOUT | EPOLLRDHUP |
                                                   EPOLLET};
                if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                  // Non-fatal error: close this connection and continue
                  log_error("epoll_ctl ADD failed. client_fd %d failed to be added "
                            "to the event loop.",
                            client_fd);
                  cm_close_connection(w, client_fd);
                  continue;
                }
    
                // TODO: Create coroutine to handle connection and schedule it.
              }
    
              // If we read fewer than the batch size, the pipe is likely drained
              if (bytes_read < (ssize_t)sizeof(fd_batch)) {
                break;
              }
            }
            continue;
          }
    
          // fd is disconnecting
          if (event & EPOLLRDHUP) {
            cm_close_connection(w, fd);
            continue;
          }
    
          // fd readable/writable
          if ((event & EPOLLIN) || (event & EPOLLOUT)) {
            wake_fd(w, fd); // thread-safe, clears everything properly
          }
        }
    
        // Continue to schedule ready coroutines
        schedule(w);
      }
    }
    ```
    
    1. **The Worker Architecture**
        
        Each worker thread runs an `epoll` instance and maintains its own coroutine scheduler. To communicate with these workers, I’ve implemented **Self-Pipes**.
        
        When the main thread `accepts` a connection, it writes the file descriptor (FD) into a worker's notification pipe. The worker’s event loop wakes up, reads the FD, registers it in its own `epoll` set, and spawns a coroutine to handle the request. This avoids the "Thundering Herd" problem—where all threads wake up for one connection—and ensures that synchronization overhead remains minimal.
        
    2. **System Call Optimization: Batching the Pipe**
        
        Under high load, the cost of switching between User Space and Kernel Space for every single connection is prohibitive. I’ve optimized the notification read to use **batching**:
        
        ```c
        int fd_batch[32];
        while (1) {
            ssize_t bytes_read = read(fd, fd_batch, sizeof(fd_batch));
            // ... calculate num_fds and process ...
        }
        ```
        
        Instead of 32 `read()` calls for 32 connections, we perform one. This significantly reduces CPU cycles spent in context switching.
        
    3. **Deep Dive: Edge-Triggered (**`EPOLLET`**) vs. Level-Triggered**
        
        I’ve opted for **Edge-Triggered (ET)** mode. Here’s the technical breakdown of why:
        
        - **Level-Triggered (Default):** The kernel tells you an FD is ready as long as there is data. If you don't read it all, `epoll_wait` will wake you up again immediately.
        - **Edge-Triggered:** The kernel only notifies you when the state *changes* (e.g., new data arrives).
        
        Using `EPOLLET` is more efficient but far more dangerous. You **must** use non-blocking FDs and you **must** read/write until you get `EAGAIN`. If you don't, you’ll never get another notification for that FD, and the connection will hang indefinitely. This is exactly why my `recv_async` and `send_async` logic (linked via the coroutines) will be critical.
        
    4. **Handling Disconnections with** `EPOLLRDHUP`
        
        Handling a "half-closed" connection can be tricky. By using the `EPOLLRDHUP` flag, the kernel notifies us if the peer closed their end of the connection (or shut down the writing half). This allows the worker to trigger `cm_close_connection` immediately, cleaning up the `epoll` registration and the coroutine before we even attempt a failed `read`.
        
    5. **The Scheduler Integration**
        
        The heartbeat of the worker is the loop that bridges `epoll` and our coroutines:
        
        1. **`epoll_wait`**: Blocks the thread until I/O occurs.
        2. **`wake_fd`**: When an event occurs, we find the coroutine waiting on that FD and move it to the `ready_queue`.
        3. **`schedule()`**: Once all I/O events are processed, we run all "Ready" coroutines until they either finish or yield (e.g., because they hit `EAGAIN`).
    
    Current Worker Flow:
    
    | **Stage** | **Action** |
    | --- | --- |
    | **Notification** | Read new FDs from pipe, create coroutine, register in `epoll`. |
    | **I/O Event** | Identify `EPOLLIN` or `EPOLLOUT`, wake the associated coroutine. |
    | **Execution** | Run ready coroutines. They perform `recv/send` until blocking. |
    | **Cleanup** | `EPOLLRDHUP` or finished coroutines trigger FD removal and memory free. |
    
    ---
    
    ```c
    void cm_close_connection(Worker *w, int client_fd) {
      if (client_fd < 0 || client_fd >= FD_SETSIZE)
        return;
    
      if (DEBUG) {
        // Declare structures for client data
        struct sockaddr_storage remote_addr; // client address information
        socklen_t sin_size =
            sizeof remote_addr;           // size of client address information
        char remote_ip[INET6_ADDRSTRLEN]; // client ip information
    
        // Retrieve client info for logging
        if (getpeername(client_fd, (struct sockaddr *)&remote_addr, &sin_size) ==
            0) {
          inet_ntop(remote_addr.ss_family,
                    get_in_addr((struct sockaddr *)&remote_addr), remote_ip,
                    INET6_ADDRSTRLEN);
          log_trace("[-] Disconnecting client (%s:%d) from socket %d", remote_ip,
                    ntohs(((struct sockaddr_in *)&remote_addr)->sin_port),
                    client_fd);
        }
      }
    
      // Networking Cleanup first
      // Remove from epoll BEFORE closing the socket to avoid race conditions
      epoll_ctl(w->epfd, EPOLL_CTL_DEL, client_fd, NULL);
      shutdown(client_fd, SHUT_WR); // Bye!
      close(client_fd);
    }
    ```
    
    In `cm_close_connection`, I’m calling `epoll_ctl(..., EPOLL_CTL_DEL, ...)` *before* `close(fd)`. This is a best practice. While Linux automatically removes closed FDs from epoll sets, explicitly deleting them prevents race conditions where an FD might be reused by the system before the event loop has fully processed its removal.
    

---

### The Dispatcher Update: Multi-Worker Round Robin

```c
// This is called by the main thread to hand off a descriptor
void cm_dispatch_connection(int client_fd) {
	// ADD THIS
  if (!worker_pool)
    return;

  make_socket_nonblocking(client_fd);

  if (DEBUG) {
    // Declare structures for client data
    struct sockaddr_storage remote_addr; // client address information
    socklen_t sin_size =
        sizeof remote_addr;           // size of client address information
    char remote_ip[INET6_ADDRSTRLEN]; // client ip information

    // Retrieve client info for logging
    if (getpeername(client_fd, (struct sockaddr *)&remote_addr, &sin_size) ==
        -1) {
      perror("getpeername");
      return;
    }
    inet_ntop(remote_addr.ss_family,
              get_in_addr((struct sockaddr *)&remote_addr), remote_ip,
              INET6_ADDRSTRLEN);

    // Log new connection
    log_trace("[+] New client connection (%s:%d) on socket %d", remote_ip,
              ntohs(((struct sockaddr_in *)&remote_addr)->sin_port), client_fd);
  }

	// ADD THIS
  // Round-robin over worker pool
  int target = next_worker_idx % total_workers;
  next_worker_idx++;

	// ADD THIS
  // Sends a message to a target worker thread via its notify pipe containing
  // the new clients socket. The worker thread registers the socket and polls
  // for writes/reads
  write(worker_pool[target].notify_fds[1], &client_fd, sizeof(int));
}
```

To utilize multiple CPU cores, I’ve implemented a thread pool. The main thread's only job is to `accept()` connections and dispatch them to a worker.

I’m using a **Round-Robin** strategy for load balancing, but the communication mechanism is what's interesting: **Self-Pipes**.

- Each worker has a `notify_fds` pipe registered in its own `epoll` instance.
- The main thread `write()`s the new `client_fd` into the pipe.
- The worker’s `epoll_wait` wakes up, reads the FD, and takes ownership.

This avoids the "Thundering Herd" problem where multiple threads wake up for a single event, and it keeps thread synchronization overhead to a minimum since workers mostly operate on their own data.

### Create and schedule coroutine

First we need to create a function that the coruotine will invoke upon initialization. This function is what will handle the request.

In the `src/` folder, create a folder called `request_handler/` and within that folder create a file called `request_handler.c`:

```
📂 httplite/
├── 📂 src/
│   ├── 📂 request_handler/
│   │   └── 📄 request_handler.c
│   └── 📄 main.c
└── ...
```

```c
#include "../../include/connection_manager.h"

/*
 * This is the entry point for every coroutine/connection (each connection maps
 * to a coroutine). This entry function is the coroutine's lifetime boundary. It
 * should own the state. This request struct must be allocated once and managed
 * by the coroutine. We must ensure we pass the Request* to parse. From this
 * point forward, we should use our async methods for read/revc, write/send,
 * sleep, etc. From this point forward, fatal errors should not quit the server
 * program but close the connection.
 */
void entry(void *arg, Worker *w) {}
```

We want to expose this `entry` function to our connection manager. Let’s create it’s interface with a header file. In the `includes/` folder, create a file called `request_handler.h`:

```
📂 httplite/
├── 📂 includes/
│   └── 📄 request_handler.h
└── ...
```

📄 `includes / request_handler.h`:

```c
#ifndef REQUEST_HANDLER_PUBLIC_H
#define REQUEST_HANDLER_PUBLIC_H

#include "connection_manager.h"

void entry(void *, Worker *);

#endif // !REQUEST_HANDLER_PUBLIC_H
```

Now in `connection_manager.c` include the header file:

```c
#include "../../include/request-handler.h"
```

Now we can Create coroutine to handle connection, every corouting (i.e. every connection) should now invoke the `entry` function.

```c
// ...            
            // Register client fd in event loop. We register interest for the
            // following events: READBLE (EPOLLIN). WRITETABLE (EPOOLOUT), and
            // DISCONNECTING (EPOLLRDHUP).
            // EPOLLET ensures that we dont perform a busy wait but actually
            struct epoll_event ev = {.data.fd = client_fd,
                                     .events = EPOLLIN | EPOLLOUT | EPOLLRDHUP |
                                               EPOLLET};
            if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
              // Non-fatal error: close this connection and continue
              log_error("epoll_ctl ADD failed. client_fd %d failed to be added "
                        "to the event loop.",
                        client_fd);
              cm_close_connection(w, client_fd);
              continue;
            }

            // ADD THIS: Create coroutine to handle connection and schedule it.
						Coroutine *co =
                coroutine_create(entry, (void *)(uintptr_t)client_fd, w);
            if (co) {
              add_to_ready(w, co);
            } else {
              // Non-fatal error: close this connection and continue
              log_error("Coroutine creation failed for client_fd %d",
                        client_fd);
              cm_close_connection(w, client_fd);
            }
          }

          // If we read fewer than the batch size, the pipe is likely drained
          if (bytes_read < (ssize_t)sizeof(fd_batch)) {
            break;
          }
        }
        continue;
      }
      
// ...
```

---

**Next time, I'll be diving into the `async.c` implementation to show how we actually handle the partial reads/writes required by the Edge-Triggered mode.**
