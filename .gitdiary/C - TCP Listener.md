# The Foundation

Welcome to the first entry of my devlog! I’ve decided to take the plunge into systems programming by building a lightweight HTTP server in C. No heavy frameworks, no high-level abstractions—just raw sockets and POSIX APIs.

Today, I tackled the most critical part of any server: **The TCP Listener.**

---

## Understanding TCP

While the Internet Protocol (IP) handles the routing of individual packets (datagrams) from host to host, it is inherently "best-effort" and unreliable. Packets can be lost, delayed, duplicated, or arrive out of order. **TCP (Transmission Control Protocol)** sits atop IP to provide a connection-oriented, reliable **byte-stream service**.

1. **The Byte-Stream Abstraction**
    
    The most critical distinction of TCP is that it does not preserve record boundaries. Unlike UDP, where one `send()` equals one packet, TCP views data as an unstructured, continuous sequence of 8-bit bytes.
    
    The application on one end writes a stream of bytes into a TCP socket; the TCP implementation buffers this data, segments it into **Maximum Segment Size (MSS)** chunks, encapsulates them in IP datagrams, and transmits them. The receiving TCP peer then reassembles these bytes into its own buffer, allowing the receiving application to read them exactly as they were sent.
    
2. **Reliability through Positive Acknowledgment with Retransmission (PAR)**
    
    TCP ensures that every byte is received by using a sequence-numbering system. Every byte in a TCP connection has its own unique **Sequence Number**.
    
    - **Cumulative Acknowledgments:** When the receiver sends an **ACK**, the acknowledgment number indicates the sequence number of the *next* byte the receiver expects to receive.8 This implicitly confirms that every byte *prior* to that number has been successfully processed.
    - **Retransmission Timers:** If the sender does not receive an ACK for a transmitted segment within a calculated **Retransmission Time-Out (RTO)**, it assumes the packet was lost and resends the data.
    - **Flow and Congestion Control:** TCP uses a "sliding window" mechanism. The receiver advertises a **Window Size**, telling the sender how much data it can buffer. This prevents a fast sender from overwhelming a slow receiver.
3. **The Three-Way Handshake**
    
    Before any HTTP data can be exchanged, TCP must establish a logical connection. This synchronization of sequence numbers is known as the **Three-Way Handshake**:
    
    1. **SYN:** The client picks an Initial Sequence Number (`ISN_c`) and sends a segment with the `SYN` flag set.
    2. **SYN-ACK:** The server increments the client's `ISN_c` to create an ACK number, picks its own `ISN_s`, and sends a segment with both `SYN` and `ACK` flags.
    3. **ACK:** The client acknowledges the server’s `ISN_s`. 
    
    Only after this "handshake" is complete is the connection considered `ESTABLISHED`, allowing the first byte of your HTTP request to be sent.

## Create the Listener socket

In the `src/` folder, create a folder called `server/` and within that folder create a file called `server.c`:

```
📂 httplite/
├── 📂 src/
│   ├── 📂 server/
│   │   └── 📄 server.c
│   └── 📄 main.c
└── ...
```

`src / server / server.c`:

```c
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BACKLOG 10 // Max number of pending connections in the queue

// create and return a listening socket
int create_listener_socket(const char *port) {
  struct addrinfo hints, *servinfo;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE; // use my IP

  int rv = getaddrinfo(NULL, port, &hints, &servinfo);
  if (rv) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
    exit(EXIT_FAILURE);
  }

  int sockfd;
  int yes = 1;
  struct addrinfo *p;
  // loop through all the results and bind to the first we can
  for (p = servinfo; p != NULL; p = p->ai_next) {
    sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (sockfd == -1) {
      perror("server: socket");
      continue;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
      perror("setsockopt");
      exit(EXIT_FAILURE);
    }

    if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
      close(sockfd);
      perror("server: bind");
      continue;
    }

    break;
  }

  freeaddrinfo(servinfo); // all done with this structure

  if (p == NULL) {
    fprintf(stderr, "server: failed to bind\n");
    exit(EXIT_FAILURE);
  }

  if (listen(sockfd, BACKLOG) == -1) {
    perror("listen");
    exit(EXIT_FAILURE);
  }

  return sockfd;
}
```

### 1. Setting the Stage with `getaddrinfo`

Instead of hardcoding IP addresses or port formats, I used `getaddrinfo`. It’s the modern way to handle network addresses because it’s protocol-agnostic. Whether the user is on IPv4 or IPv6, the `hints` structure ensures we get the right kind of socket.

### 2. The Socket "Ritual"

Creating a listening socket feels like a specific ritual you have to perform perfectly for the OS to grant you access:

- **`socket()`**: Get a file descriptor (a handle for the network).
- **`setsockopt()`**: This is a lifesaver. I set `SO_REUSEADDR` so that if I restart the server, I don't have to wait for the OS to "clean up" the port before I can use it again.
- **`bind()`**: This is where the server officially "claims" the port on the local machine.
- **`listen()`**: We tell the OS we are ready to receive incoming connections and set a `BACKLOG` for how many pending connections can wait in line.

## The Main Accept Loop & Handoff

Then add the server init:

```c
#include "../../include/liblog.h"

// Initialize Server
void server_init(const char *port) {
  // Declare structures for client data
  struct sockaddr_storage remote_addr;     // client address information
  socklen_t sin_size = sizeof remote_addr; // size of client address information

  // Create listener socket descriptor
  int sockfd = create_listener_socket(port);
  log_info("[*] Server listening on port %s", port);

  // Accept() loop
  while (1) {
    int client_fd = accept(sockfd, (struct sockaddr *)&remote_addr,
                           &sin_size); // accept new connection
    if (client_fd == -1)
      continue;

    // Here we handoff the connection to the connection manager we will build soon...
  }
}
```

The `server_init` function contains the heartbeat of the server: the `while(1)` loop.

Currently, the logic is clean:

1. **Accept** a new connection.
2. **Dispatch** that connection to a `connection_manager`.

`server_init` is the function we want to expose to our main function to initialize the server. Let’s create its interface with a header file.

In the `includes/` folder, create a file called `server.h`:

```
📂 httplite/
├── 📂 includes/
│   └── 📄 server.h
└── ...
```

📄 `includes` / `server.h`:

```c
#ifndef SERVER_H
#define SERVER_H

/* This initializes the HTTP server on the specified PORT */
void server_init(const char *);

#endif
```

Now in `server.c` include the header file:

```c
#include "../../include/server.h"
```

## The connection manager

In the `src/` folder, create a folder called `connection_manager/` and within that folder create a file called `connection_manager.c`:

```
📂 httplite/
├── 📂 src/
│   ├── 📂 connection_manager/
│   │   └── 📄 connection_manager.c
│   ├── 📂 server/
│   │   └── 📄 server.c
│   └── 📄 main.c
└── ...
```

📄 `src` / `connection_manager` / `connection_manager.c`:

```c
#include "../../include/liblog.h"
#include <arpa/inet.h>
#include <fcntl.h>

#define DEBUG 1

// make fd non-blokcing
int make_socket_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa) {
  if (sa->sa_family == AF_INET) {
    return &(((struct sockaddr_in *)sa)->sin_addr);
  }

  return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

// This is called by the main thread to hand off a descriptor
void cm_dispatch_connection(int client_fd) {
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
}
```

One interesting choice I made was setting the `O_NONBLOCK` flag in the `make_socket_nonblocking` for client fds:

```c
int make_socket_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
```

By making the clients non-blocking, the server won't "freeze" while waiting for a socket operation. This sets the stage for a more advanced architecture—likely involving `epoll` or `poll`—where we can handle thousands of connections without spawning a thousand threads.

Now in `connection_manager.c` include the header file:

```c
#include "../../include/connection_manager.h"
```

Now to expose our `cm_dispatch_connection` function to our server, let’s create its interface with a header file. In the `include/` folder, create a file called `connection_manager.h`:

```
📂 httplite/
├── 📂 include/
│   └── 📄 connection_manager.h
└── ...
```

📄 `includes` / `connection_manager.h`:

```c
#ifndef CONNECTION_MANAGER_PUB_H
#define CONNECTION_MANAGER_PUB_H

// Connection Management
void cm_dispatch_connection(int client_fd);

#endif
```

Now we want to call the `cm_dispatch_connection` when we’ve accepted a connection from a client. To do so, In `server.c` include the header file:

```c
#include "../../include/connection_manager.h"
```

Then in the `server_init` function, call `cm_dispatch_connection` after accepting a connection:

```c
// Initialize Server
void server_init(const char *port) {
  // Declare structures for client data
  struct sockaddr_storage remote_addr;     // client address information
  socklen_t sin_size = sizeof remote_addr; // size of client address information

  // Create listener socket descriptor
  int sockfd = create_listener_socket(port);
  log_info("[*] Server listening on port %s", port);

  // Accept() loop
  while (1) {
    int client_fd = accept(sockfd, (struct sockaddr *)&remote_addr,
                           &sin_size); // accept new connection
    if (client_fd == -1)
      continue;

    // ADD THIS: Simple, clean handoff to connection manager
    cm_dispatch_connection(client_fd);
  }
}
```

By passing the `client_fd` to `cm_dispatch_connection`, I’m keeping the networking logic separate from the request-handling logic. This separation of concerns will make it much easier to scale the server later.

---

## The `main` function

In `main` use the `server_init` function to start the server on a given port:

```c
 ============================================================================
 Name        : HTTP Server
 Author      : Fullname
 Version     : 1.0
 Description : An event-driven HTTP Server written in pure C.
 License     : MIT
 ============================================================================
*/

#include "../include/server.h"

#define PORT "8080" // Port number

int main(void) { server_init(PORT); }
```

## The Listener in Action

Now compiler and run the project using the Makefile:

```bash
❯ ****make run
# ...
15:11:02 INFO  src/server/server.c:80: [*] Server listening on port 3094
```

In another terminal, start connections to the server using `netcat`:

```bash
❯ ****netcat localhost 3094
```

You should see logs from the server like this

```bash
15:11:02 INFO  src/server/server.c:80: [*] Server listening on port 3094
15:11:05 TRACE src/connection_manager/connection_manager.c:101: [+] New client connection (127.0.0.1:50848) on socket 4
15:11:11 TRACE src/connection_manager/connection_manager.c:101: [+] New client connection (127.0.0.1:50850) on socket 5
15:11:21 TRACE src/connection_manager/connection_manager.c:101: [+] New client connection (127.0.0.1:50508) on socket 6
15:11:27 TRACE src/connection_manager/connection_manager.c:101: [+] New client connection (127.0.0.1:55370) on socket 7
```

---

### What’s Next?

Now that the server can hear "knocks" on the door, it needs to learn how to talk. My next step is implementing the **Connection Manager** to read incoming HTTP strings and eventually parse them into something meaningful.
