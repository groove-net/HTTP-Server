#include "../../../include/connection_manager.h"
#include "../request_handler.h"
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
