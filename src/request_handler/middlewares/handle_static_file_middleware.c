#include "../../../include/connection_manager.h"
#include "../request_handler.h"
#include "http_utils.c"

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
