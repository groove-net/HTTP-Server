### HTTP Utilities: Redirects, Zero-Copy, and TCP Corking

In the `src/request_handler/` folder, create a file called `http_utils.c`:

```
📂 httplite/
├── 📂 src/
│   ├── 📂 request_handler/
│   │   └── 📄 http_utils.c
│   └── 📄 main.c
└── ...
```

`src` / `request_handler` / `http_utils.c`:

```c
#include "request_handler.h"
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
  const char *ext;
  const char *type;
} MimeMap;

MimeMap mime_types[] = {
    {".html", "text/html"},        {".htm", "text/html"},
    {".css", "text/css"},          {".js", "application/javascript"},
    {".png", "image/png"},         {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},       {".gif", "image/gif"},
    {".txt", "text/plain"},        {NULL, "application/octet-stream"},
    {".svg", "image/svg+xml"},     {".ico", "image/x-icon"},
    {".json", "application/json"}, {".map", "application/json"}};

const char *get_mime_type(const char *path) {
  char *ext = strrchr(path, '.');
  if (!ext)
    return mime_types[9].type;
  for (int i = 0; mime_types[i].ext != NULL; i++) {
    if (strcmp(ext, mime_types[i].ext) == 0)
      return mime_types[i].type;
  }
  return mime_types[9].type;
}

/**
 * @brief Searches for a header value by key (case-insensitive).
 * @param req Pointer to the parsed Request.
 * @param key The header name to search for (e.g., "Connection").
 * @return Pointer to the value string if found, NULL otherwise.
 */
const char *get_header(const Request *req, const char *key) {
  if (!req || !key)
    return NULL;

  for (int i = 0; i < req->header_count; i++) {
    // HTTP/1.1 headers are case-insensitive per RFC 7230
    if (strcasecmp(req->headers[i].key, key) == 0) {
      return req->headers[i].value;
    }
  }
  return NULL;
}

void serve_file(int client_fd, const char *path, off_t file_size,
                const char *method, int keep_alive, Worker *w) {
  const char *mime = get_mime_type(path);
  char header[512];
  int state = 1;

  // 1. "Cork" the socket: Stop the kernel from sending partial packets
  setsockopt(client_fd, IPPROTO_TCP, TCP_CORK, &state, sizeof(state));

  // Build HTTP/1.1 Header
  int header_len =
      snprintf(header, sizeof(header),
               "HTTP/1.1 200 OK\r\n"
               "Content-Type: %s\r\n"
               "Content-Length: %ld\r\n"
               "Connection: %s\r\n"
               "\r\n",
               mime, file_size, keep_alive ? "keep-alive" : "close");

  // Use your send_async for the header
  send_async(client_fd, header, header_len, 0, w);

  // Only send the body if it's NOT a HEAD request
  if (strcmp(method, "HEAD") != 0) {
    sendfile_async(client_fd, path, file_size, w);
  }

  // 4. "Uncork" the socket: Flush everything out to the network at once
  state = 0;
  setsockopt(client_fd, IPPROTO_TCP, TCP_CORK, &state, sizeof(state));
}

void send_redirect(int client_fd, const char *old_uri, int keep_alive,
                   Worker *w) {
  char buf[512];
  char new_location[MAX_LINE_LEN];

  // Add the trailing slash
  snprintf(new_location, sizeof(new_location), "%s/", old_uri);

  int len = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 301 Moved Permanently\r\n"
                     "Location: %s\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     new_location, keep_alive ? "keep-alive" : "close");

  // Use your async send to ensure the worker stays non-blocking
  send_async(client_fd, buf, len, 0, w);
}

const char *get_http_reason(int code) {
  switch (code) {
  case 400:
    return "Bad Request";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 500:
    return "Internal Server Error";
  default:
    return "Unknown Error";
  }
}
void send_error(int client_fd, int code, const char *msg, int keep_alive,
                Worker *w) {
  const char *reason = get_http_reason(code);

  // 1. Prepare the body and determine if we need a newline
  const char *body_content = msg ? msg : "";
  const char *newline = (msg && strlen(msg) > 0) ? "\n" : "";

  // 2. Calculate actual Content-Length (body + optional newline)
  size_t content_len = strlen(body_content) + strlen(newline);

  // 3. Build the full response
  char response[1024];
  int total_len =
      snprintf(response, sizeof(response),
               "HTTP/1.1 %d %s\r\n"
               "Content-Type: text/plain\r\n"
               "Content-Length: %zu\r\n"
               "Connection: %s\r\n"
               "\r\n"
               "%s%s", // Body followed by optional newline
               code, reason, content_len, keep_alive ? "keep-alive" : "close",
               body_content, newline);

  // Safety check for truncation
  if (total_len >= (int)sizeof(response)) {
    total_len = sizeof(response) - 1;
  }

  send_async(client_fd, response, total_len, 0, w);
}
```

With the core networking and coroutine runtime in place, I needed a suite of utilities to handle the "language" of HTTP. This entry covers how the server manages URI normalization, optimizes file transfers with `sendfile`, and uses `TCP_CORK` to minimize packet overhead.

### 1. MIME Mapping and Header Lookups

Since a browser needs to know whether it's receiving an image, a stylesheet, or an HTML document, I implemented a simple `MimeMap` lookup. It defaults to `application/octet-stream` if an extension is unknown—forcing a download rather than a broken render.

I also added a `get_header` utility. Per **RFC 7230**, HTTP headers are case-insensitive. Using `strcasecmp` ensures that `Content-Type`, `content-type`, and `CONTENT-TYPE` are all treated as the same key, making the server compliant with standard browser behavior.

### 2. Normalizing URIs: The 301 Redirect

A **301 Moved Permanently** redirect is the standard way to tell a browser: *"You asked for a directory, but you forgot the trailing slash."* This is crucial for fixing relative links.

- **The Problem:** If a user visits `/docs` and the HTML contains `<img src="logo.png">`, the browser tries to fetch `/logo.png` (the root).
- **The Solution:** By redirecting the user to `/docs/`, the browser understands it is "inside" a folder and correctly fetches `/docs/logo.png`.

The `send_redirect` function builds a minimal HTTP response with the `Location` header to handle this automatically.

### 3. Zero-Copy Performance: `sendfile()`

For static file serving, I implemented `serve_file` using the Linux `sendfile()` system call. Traditionally, serving a file requires multiple context switches and data copies:

> Disk -> Kernel Buffer -> App Buffer -> Kernel Socket Buffer -> Network
> 

With **Zero-Copy** via `sendfile()`, the data stays in kernel space:

> Disk -> Kernel Buffer -> Kernel Socket Buffer -> Network
> 

By bypassing application memory, the server achieves massive throughput. Combined with our coroutine architecture, `sendfile_async` ensures that if the outbound TCP window is full (`EAGAIN`), the coroutine "parks" itself, allowing the worker thread to service other clients until the buffer clears.

### 4. The "CORK" Technique

To further optimize small file delivery, I utilized `TCP_CORK`. Web apps often request many small files (JS, CSS, icons) simultaneously.

```c
int state = 1;
setsockopt(client_fd, IPPROTO_TCP, TCP_CORK, &state, sizeof(state));
// ... send headers ...
// ... send file body via sendfile ...
state = 0;
setsockopt(client_fd, IPPROTO_TCP, TCP_CORK, &state, sizeof(state));
```

Normally, the kernel might send headers in one packet and the body in another. By "corking" the socket, I force the kernel to accumulate both into a single, full-sized MTU packet. This reduces the total number of packets sent across the wire, significantly improving efficiency for modern SPAs.

### 5. Graceful Error Handling

Finally, I built a `send_error` dispatcher that maps common status codes (404, 500, etc.) to their human-readable "Reason Phrases." It uses `snprintf` to safely build response strings, ensuring that even under error conditions, the server remains stable and secure.
