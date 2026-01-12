## Scaling for Concurrency

Right now, we have a listener functioning on the main thread, but any server worth its salt must handle multiple requests simultaneously. The question is: how do we manage these concurrent client connections efficiently? There are two primary architectural models to consider:

### **The Thread-per-Request Model**

Traditionally, servers were built using a "thread-per-request" approach. In this model, each incoming connection spawns a new OS thread (or pulls one from a pre-allocated thread pool). That dedicated thread handles the entire lifecycle of the request—reading I/O, processing logic, and sending the response. This model is intuitive because it relies on **blocking I/O**: the thread simply waits for syscalls like `read()` or `accept()` to complete.

While simpler to implement, this model suffers from significant overhead under heavy loads. Each thread consumes kernel resources and memory for its own stack space. As the number of simultaneous clients grows, performance degrades due to constant context switching and thread limits. While this could work for a learning project or a low-traffic server (fewer than 100 concurrent clients), it is not a scalable solution for modern web traffic.

### **The Event-Driven Model (Non-blocking I/O)**

To achieve high performance and handle thousands of simultaneous connections, modern production-grade systems like NGINX and Node.js use an **event-driven model**. This approach uses a minimal number of threads to manage many connections asynchronously through non-blocking I/O and event notification mechanisms like `poll()` or `epoll()`. Instead of one thread per client, the server maintains an **Event Loop** that monitors all active sockets. The loop waits for specific "events"—such as a new connection or a socket being ready for reading—and dispatches them for processing only when action is required.

### **Our Path Forward: Hybrid Asynchrony**

We will implement the event-driven model to ensure our server remains lightweight and performant. However, a single-threaded event loop introduces a new challenge: any long-running or blocking operation will "freeze" the entire loop, preventing other clients from being serviced. To solve this without the overhead of massive threading, we will introduce **Coroutines**. This will allow us to write code that looks sequential and easy to reason about, while the underlying system handles multitasking cooperatively, ensuring our event loop remains fast and responsive.
