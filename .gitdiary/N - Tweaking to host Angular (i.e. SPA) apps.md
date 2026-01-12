Since your server now supports **static file serving**, **MIME types**, and **directory indexing**, it has everything an Angular "dist" folder needs to run.

However, there is one major requirement for Single Page Applications (SPAs) like Angular that we need to handle: **Client-Side Routing**.

### The "Deep Link" Problem

In an Angular app, if you visit `http://localhost:8080/`, your server serves `index.html`, Angular loads, and everything is fine.

But if the user refreshes the page while on `http://localhost:8080/dashboard`, the browser sends a request to your C server for `/dashboard`.

- **Current Server:** Looks for `public/dashboard`, doesn't find a file, and returns a **404**.
- **Requirement:** For an SPA, the server must "fallback" and serve `index.html` for any route it doesn't recognize, allowing Angular's internal router to take over.

### Updating your Middleware for Angular

We can modify your `handle_static_file_middleware` to act as an SPA host. If the file isn't found, instead of a 404, we serve `index.html`.

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
	
  // ADD THIS:
  // SPA FALLBACK: If file not found, check if it's a "deep link"
  // We only fallback for GET requests that don't look like files (no dot in the
  // last segment)
  char *dot = strrchr(req->uri, '.');
  if (!dot) {
    // It's a route like /dashboard, serve the main index.html
    snprintf(path, sizeof(path), "%s/index.html", root);
    if (stat(path, &st) == 0) {
      serve_file(client_fd, path, st.st_size, req->method, keep_alive, w);
      return;
    }
  }

  // If we get here, something failed
  send_error(client_fd, 404, NULL, keep_alive, w);
}
```

### How to Deploy

1. **Build your App:** Run `npm run build -- --configuration production` in your Angular project.
2. **Copy Files:** Take everything inside the `dist/<project-name>/browser` folder (or wherever your `index.html` ended up) and put it into your C server's `public/` folder.
    
    ```bash
    cp ./dist/<project-name>/browser/* ~/Work/http_server/public
    ```
    
3. **Run:** Start your C server.

### Why this server is actually great for Angular:

- **Javascript MIME Type:** Our `.js` mapping ensures the browser executes the Angular bundles correctly.
- **Keep-Alive:** Angular apps usually load 5–10 scripts and CSS files at once. Our new Keep-Alive logic will make the app load **significantly faster** because it reuses the TCP connection for all those `.js` files.
- **Concurrency:** Since Angular makes many small requests for assets, our worker pool will handle the "burst" of requests very efficiently.
