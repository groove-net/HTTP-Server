### The Static File Router

This function translates a URI (like `/about`) into a disk path (like `./public/about.html`) and handles the `index.html` default.

```c
#include "../http_utils.c"
#include <sys/stat.h>

void handle_static_file_middleware(Request *req, int keep_alive, Worker *w,
                                   int client_fd) {
  // 1. Strict Method Check
  // Standard static file servers only respond to GET and HEAD
  if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "HEAD") != 0) {
    // If you have an API, you'd pass the request along here.
    // Otherwise, return 405 Method Not Allowed.
    send_error(client_fd, 405, NULL, keep_alive, w);
    return;
  }

  char path[MAX_LINE_LEN];
  struct stat st;

  // Use a clean root. Ensure no trailing slash.
  const char *root = "./public";

  // Safety: Prevent Directory Traversal (Basic)
  if (strstr(req->uri, "..")) {
    send_error(client_fd, 400, NULL, keep_alive, w);
    return;
  }

  // If URI is just "/", treat it as an empty string for the path builder
  const char *uri = (strcmp(req->uri, "/") == 0) ? "/index.html" : req->uri;

  log_trace("URI = %s", uri);

  // Create full file path
  if (uri[0] == '/')
    snprintf(path, sizeof(path), "%s%s", root, uri);
  else
    snprintf(path, sizeof(path), "%s/%s", root, uri);

  // Handle request
  if (stat(path, &st) == 0) {
    // --- DIRECTORY HANDLING ---
    if (S_ISDIR(st.st_mode)) {
      size_t len = strlen(req->uri);
      // If it's a directory and DOES NOT end in '/', redirect
      if (len > 0 && req->uri[len - 1] != '/') {
        // Send a 301 Redirect to the version with the slash
        // This ensures the browser's relative path logic stays correct
        send_redirect(client_fd, req->uri, keep_alive, w);
        return;
      }
      // Otherwise, append index.html and try again
      strncat(path, "/index.html", sizeof(path) - strlen(path) - 1);
      if (stat(path, &st) != 0) {
        send_error(client_fd, 404, NULL, keep_alive, w);
        return;
      }
    }

    // --- FILE SERVING ---
    if (S_ISREG(st.st_mode)) {
      serve_file(client_fd, path, st.st_size, req->method, keep_alive, w);
      return;
    }
  }

  // If we get here, something failed
  send_error(client_fd, 404, NULL, keep_alive, w);
}

```

Normalizing paths if you want to:

```c
void normalize_path(char *path) {
    char *src = path, *dst = path;
    while (*src) {
        *dst = *src;
        // If we just wrote a slash, skip all subsequent slashes in the source
        if (*src == '/') {
            while (*(src + 1) == '/') src++;
        }
        src++;
        dst++;
    }
    *dst = '\0';
}
```

### Integration into your middleware

The URI decoder middleware (in `uri_decoder_middleware.c`) should now call the static file middleware at the end before returning:

```c
  
  
  // go next middleware
  handle_static_file_middleware(req, keep_alive, w, client_fd); // ADD THIS
}
```

### Key Improvements for "Lean" Performance:

1. **Direct Header Injection:** By building the response headers in a local buffer, you avoid complex string manipulation libraries.
2. **`stat()` Before `open()`:** We verify the file exists and is a regular file before allocating a file descriptor for it.
3. **MIME Table:** The extension-based lookup is `O(n)` where $n$ is very small, making it effectively `O(1)` without the complexity of a hash map.

**Next Step:** Would you like to implement **`sendfile()`**? It is a Linux-specific system call that would make your `serve_file` significantly faster by avoiding copying the file data into your program's memory entirely.

### Browser Testing

It should work perfectly in the browser now! As long as your server is listening (e.g., on port 8080) and you have a folder named `public` in your execution directory, the browser will render this HTML.

Here is a clean, modern **index.html** file designed specifically to test your server's capabilities:

```html
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>C-Server Test</title>
    <style>
        body {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
            line-height: 1.6;
            color: #333;
            max-width: 800px;
            margin: 40px auto;
            padding: 0 20px;
            background-color: #f4f7f6;
        }
        .card {
            background: white;
            padding: 30px;
            border-radius: 8px;
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
        }
        h1 { color: #2c3e50; border-bottom: 2px solid #3498db; padding-bottom: 10px; }
        .status-badge {
            display: inline-block;
            background: #27ae60;
            color: white;
            padding: 5px 12px;
            border-radius: 20px;
            font-size: 0.8em;
            font-weight: bold;
        }
        code {
            background: #2c3e50;
            color: #ecf0f1;
            padding: 2px 6px;
            border-radius: 4px;
            font-family: "Courier New", Courier, monospace;
        }
    </style>
</head>
<body>
    <div class="card">
        <h1>Server Status: <span class="status-badge">ONLINE</span></h1>
        <p>If you are seeing this page, your <strong>C-based Coroutine Server</strong> is successfully:</p>
        <ul>
            <li>Accepting TCP connections via the main thread.</li>
            <li>Dispatching FDs through the notification pipe.</li>
            <li>Scheduling coroutines on worker threads.</li>
            <li>Parsing HTTP requests using your state machine.</li>
            <li>Serving static files from disk with correct MIME types.</li>
        </ul>
        <p>Try requesting a non-existent file: <a href="/notfound.html">Test 404 Page</a></p>
        <hr>
        <p><small>Served by: <code>HTTP_Server</code></small></p>
    </div>
</body>
</html>
```

### Quick Test Instructions

1. **Create the Directory:** Run `mkdir public` in your project folder.
2. **Save the File:** Save the code above as `public/index.html`.
3. **Run your Server:** Start your binary using the make command (ensure it's bound to a port, let's say `8080`).
    
    ```bash
    make run
    ```
    
4. **Open Browser:** Go to `http://localhost:8080/`.

### What to watch for in your logs:

When you hit the page in a browser, you might see **two** requests in your logs:

1. `GET /` (Which your router should resolve to `/index.html`)
2. `GET /favicon.ico` (Browsers ask for this automatically).
