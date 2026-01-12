# httplite

A high-performance, lightweight HTTP/1.1 server written in C. **httplite** is designed as a deep dive into systems programming, utilizing a multi-threaded event loop, cooperative multitasking (coroutines), and zero-copy I/O.

## 🚀 Key Features

- **Cooperative Multitasking:** Custom coroutine runtime built on `ucontext.h` for managing thousands of concurrent connections with a sequential programming model.
- **Multi-threaded Event Loop:** A "Worker Pool" architecture using Linux `epoll` in Edge-Triggered (EPOLLET) mode for maximum CPU scalability.
- **Zero-Copy I/O:** Leverages `sendfile()` to stream data directly from the kernel Page Cache to the network socket, bypassing user-space memory.
- **Efficient Dispatching:** Uses "Self-Pipes" and a Round-Robin strategy to distribute connections from the main listener to worker threads with minimal lock contention.
- **Modern SPA Support:** Optimized for hosting Single Page Applications (like Angular/React) with automatic URI normalization and 301 redirects.

---

## 🏗 Architecture Overview

The server operates on a **Leader-Follower** pattern:

1. **The Listener (Main Thread):** A dedicated thread that `accept()`s new TCP connections and dispatches the file descriptors to the worker pool.
2. **The Workers (Pool):** Each worker thread runs its own `epoll` instance and a coroutine scheduler.
3. **The Coroutines:** Each connection is mapped to a coroutine. When an I/O operation (like `recv`) would block, the coroutine yields control back to the worker, allowing other requests to proceed.

---

## 🛠 Prerequisites

- **OS:** Linux (Requires `epoll`, `sendfile`, and `ucontext.h`)
- **Compiler:** `clang` or `gcc` (with C99 support)
- **Dependencies:** `pthread`

---

## 💻 Getting Started

### 1. Build and Run

The project uses a custom Makefile designed for modular C builds:

```c
make
./bin/program
```

### 2. Benchmarking

To test the concurrency limits, it is recommended to use `wrk`:

```c
wrk -t12 -c400 -d30s http://localhost:3094/
```

---

## ⚙️ Optimizations

### TCP Corking

To reduce packet overhead, **httplite** uses `TCP_CORK`. This "corks" the socket while headers and file bodies are being prepared, forcing the kernel to coalesce them into a single MTU-sized packet before transmission.

### Pre-compressed Gzip

The server supports a "lean" compression strategy. If a browser sends `Accept-Encoding: gzip`, **httplite** looks for a `.gz` version of the file on disk and serves it directly, saving CPU cycles compared to on-the-fly compression.

---

## 📂 Project Structure

- `src/server/`: Core TCP listener logic.
- `src/connection_manager/`: Coroutine runtime, scheduler, and `epoll` worker loop.
- `src/request_handler/`: HTTP parsing and middleware pipeline.
- `include/`: Public headers and interface definitions.

---

## 📜 License

This project is licensed under the MIT License - see the LICENSE.md file for details.
