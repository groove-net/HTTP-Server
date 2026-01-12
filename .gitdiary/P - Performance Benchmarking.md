### **Stress Testing**

Use a tool like `wrk` or `ab` (Apache Benchmark) to see how many thousands of requests per second your new engine can handle.

- Installing `wrk` using an AUR-Helper on Arch Linux
    
    ```bash
    yay -S wrk
    ```
    
- Using `wrk`
    
    `wrk` is the industry standard for high-performance testing. It uses an event loop (like your server) to generate massive load.
    
    **The Command:**
    
    ```bash
    wrk -t12 -c400 -d30s http://127.0.0.1:3094/index.html
    ```
    
    **What these flags mean:**
    
    - `t12`: Use **12 threads** to generate requests. (Matches your CPU cores).
    - `c400`: Keep **400 HTTP connections** open at once. (Tests your `epoll` and coroutine switching).
    - `d30s`: Run the test for **30 seconds**.

### **Performance Tuning**

Experiment with `RDY_LIFO` (Last-In-First-Out) in your `Worker` policy. LIFO can sometimes improve performance by keeping the coroutine's data in the CPU's L1 cache between yields.

- FIFO results
    
    ```bash
    Running 30s test @ http://127.0.0.1:3094/index.html
      12 threads and 400 connections
      Thread Stats   Avg      Stdev     Max   +/- Stdev
        Latency     8.81ms   43.08ms   1.67s    99.70%
        Req/Sec     4.03k     1.45k   13.44k    80.26%
      1431542 requests in 30.10s, 8.69GB read
      Socket errors: connect 0, read 0, write 0, timeout 80
    Requests/sec:  47564.50
    Transfer/sec:    295.75MB
    ```
    
- LIFO results
    
    ```bash
    Running 30s test @ http://127.0.0.1:3094/index.html
      12 threads and 400 connections
      Thread Stats   Avg      Stdev     Max   +/- Stdev
        Latency     8.65ms   50.71ms   1.68s    99.52%
        Req/Sec     4.17k     1.90k   17.76k    72.34%
      1465002 requests in 30.10s, 8.90GB read
      Socket errors: connect 0, read 0, write 0, timeout 96
    Requests/sec:  48674.33
    Transfer/sec:    302.65MB
    ```
    

We are hitting nearly **50,000 requests per second** and pushing **300 MB/s** of throughput on a single machine. To put this in perspective, many high-level language frameworks (like Python’s Flask or Ruby on Rails) would struggle to hit 5,000 requests per second on the same hardware.

- **48,674 req/sec:** This is a very high number. It proves our **coroutine switching** is extremely lightweight. We aren't wasting CPU cycles on heavy thread context switches.
- **302 MB/s Transfer:** We are effectively saturating a significant portion of your memory and bus bandwidth. This is where our **Zero-Copy `sendfile`** and **`TCP_CORK`** are shining—we are moving 9GB of data in 30 seconds without the CPU ever having to "touch" the file bytes in user-space.

LIFO performed marginally (~2%) better than FIFO, but suffered from slightly higher timeouts. 

We have a very low **average latency (8.6ms)**, which is great, but a **Max latency of 1.6s** is a red flag for a "real-world" app. However, since 99%+ of our requests are under 10ms, that 1.6s spike is likely a "stray" coroutine that got stuck or an OS-level event (like a disk I/O block or the kernel's TCP stack buffer being momentarily full).

I had around ~80-90 timeouts. In a 30-second test with 1.4 million requests, this is very low (0.006% error rate), but it suggests that under extreme pressure, a few connections are falling through the cracks of the `epoll` event loop.

If you want to break the **60k-70k** barrier, here is what I would look at next:

1. **Heap Allocations:** Are you calling `malloc`/`free` inside `parse_http` or your middleware? At 50k req/sec, the heap lock becomes a bottleneck. Switch to a **Pool Allocator** or reuse the `Request` objects.
2. **String Comparisons:** You use `strcasecmp` for headers. Pre-hashing common headers (like `Connection`) into integers during parsing would make lookups O(1) instead of O(n).

---

### **Performance Benchmarking: Scaling to Millions**

Building the server is only half the battle. To truly understand its limits, I ran benchmarks using tools like `wrk` and `bombardier`. While our current architecture is already fast thanks to `epoll` and `sendfile`, reaching "production-grade" scale requires tuning both the code and the OS.

1. **Breaking the Kernel Limits**
2. **CPU Scalability: `SO_REUSEPORT`**
3. **Memory Optimization: Thread-Local Pools**
4. **The Next Frontier: `io_uring`**
5. **Bandwidth Efficiency: Pre-compressed Gzip**
6. **Caching: The Best I/O is No I/O**

Summary of Optimizations:

| **Optimization** | **Target** | **Impact** |
| --- | --- | --- |
| **`ulimit -n`** | OS Limits | Prevents connection drops under load. |
| **`SO_REUSEPORT`** | Parallelism | Scales listener throughput across all CPU cores. |
| **Memory Arenas** | Latency | Eliminates `malloc` lock contention in workers. |
| **`sendfile`** | I/O | Achieves zero-copy data transfer. |
| **Pre-Gzip** | Bandwidth | Reduces transfer time for large JS bundles. |

### Conclusion

And there you have it! You have successfully implemented a high-performance, asynchronous, coroutine-based HTTP server using a "Shared-Nothing" architecture., a mini-Nginx if you will This design is highly scalable because each worker thread operates independently, minimizing CPU cache thrashing and eliminating the overhead of mutex contention.

The Execution Flow (The "Happy Path")

1. **Main Thread:** Accepts a connection, calls `dispatch`.
2. **Worker Thread:** `epoll_wait` returns for the pipe. It reads the FD into `fd_batch`.
3. **Handoff:** `coroutine_create` builds the stack, and `add_to_ready` puts it in the queue.
4. **Execution:** `schedule()` swaps context to `trampoline_start` -> `entry`.
5. **I/O:** `recv_async` finds no data, calls `yield`.
6. **Yield:** `swapcontext` saves the stack/registers and returns to `schedule()`.
7. **Wake:** Data arrives, `epoll` triggers `wake_fd`. Coroutine moves back to `ready_head`.
8. **Cleanup:** Handler finishes, `trampoline_start` sets `finished = 1`. `schedule()` calls `coroutine_destroy`.
