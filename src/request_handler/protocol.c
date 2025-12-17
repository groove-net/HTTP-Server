#include "request_handler.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/**
 * @brief Allocates memory for a string and sets it in the Request struct.
 * @param src The source string to copy.
 * @return Dynamically allocated copy of the string, or NULL on failure.
 */
static char *safe_strdup(const char *src) {
  if (src == NULL)
    return NULL;
  size_t len = strlen(src) + 1;
  char *dest = (char *)malloc(len);
  if (dest) {
    memcpy(dest, src, len);
  }
  return dest;
}

/**
 * @brief Initializes a new Request object.
 */
void request_init(Request *req) {
  if (!req)
    return;
  memset(req, 0, sizeof(Request));
  req->state = NEW;
}

/**
 * @brief Cleans up dynamically allocated memory in the Request object.
 */
void request_cleanup(Request *req) {
  if (!req)
    return;
  free(req->method);
  free(req->uri);
  free(req->version);
  for (int i = 0; i < req->header_count; i++) {
    free(req->headers[i].key);
    free(req->headers[i].value);
  }
  // Note: Line buffer is on the stack, no need to free.
  request_init(req); // Reset structure to initial state
}

/**
 * @brief Appends data to the internal line buffer, looking for \r\n.
 * * @param req_co The request context.
 * @param buf The incoming data buffer pointer (will be advanced).
 * @param nbytes_ptr Pointer to remaining bytes (will be decremented).
 * @return 1 if a full line (\r\n) was found, 0 if partial, -1 on error (line
 * too long).
 */
static int read_line(Request *req_co, const char **buf_ptr, int *nbytes_ptr) {
  const char *buf = *buf_ptr;
  int nbytes = *nbytes_ptr;

  for (int i = 0; i < nbytes; i++) {
    char c = buf[i];

    if (req_co->line_len >= MAX_LINE_LEN - 1) {
      fprintf(stderr, "Error: Line buffer overflow.\n");
      req_co->state = ERROR;
      return -1; // Line too long, Bad Request
    }

    // 1. Append the character
    req_co->line_buf[req_co->line_len++] = c;

    // 2. Check for the \r\n delimiter (minimal check for efficiency)
    if (req_co->line_len >= 2 &&
        req_co->line_buf[req_co->line_len - 2] == '\r' &&
        req_co->line_buf[req_co->line_len - 1] == '\n') {

      // Null-terminate the line for C string functions
      req_co->line_buf[req_co->line_len - 2] = '\0'; // Remove \r\n
      req_co->line_len -= 2;                         // Line length without \r\n

      // Update pointers/counters for the input buffer
      *buf_ptr += (i + 1);
      *nbytes_ptr -= (i + 1);
      return 1; // Full line successfully parsed
    }
  }

  // Processed all available bytes, but no full line found
  *buf_ptr += nbytes;
  *nbytes_ptr -= nbytes;
  return 0; // Partial line
}

// Forward declarations for parsing logic
static int parse_request_line(Request *req_co);
static int parse_header_field(Request *req_co);

/**
 * @brief The main HTTP/1.1 state machine parser.
 * * @param req_co The request context.
 * @param buf Incoming data buffer.
 * @param nbytes Number of bytes in the buffer.
 * @return 1 on DONE, 0 on partial, -1 on ERROR.
 */
int parse_http(Request *req_co, const char *buf, int nbytes) {
  const char *current_buf_ptr = buf;
  int remaining_bytes = nbytes;
  int line_status = 0;

  // Loop until all input is consumed or a final state is reached
  while (remaining_bytes > 0 && req_co->state != DONE &&
         req_co->state != ERROR) {

    // Use the line reader for states that require a full line
    if (req_co->state >= NEW && req_co->state <= HEADERS) {
      line_status = read_line(req_co, &current_buf_ptr, &remaining_bytes);
    } else {
      // For BODY/other states, logic would go here.
      // Since we only implement RL and Headers, we break if a line isn't
      // needed.
      break;
    }

    // If line is incomplete or error, handle the return value
    if (line_status == 0) {
      return 0; // Need more data
    }
    if (line_status == -1) {
      return -1; // Error already set in read_line
    }

    // --- Full Line Received (line_status == 1) ---

    if (req_co->state == NEW) {
      // The first line is the Request Line
      if (parse_request_line(req_co) == -1)
        return -1;
      req_co->state = HEADERS;

    } else if (req_co->state == HEADERS) {

      if (req_co->line_len == 0) {
        // Empty line (\r\n\r\n) marks the end of headers
        req_co->state = BODY_START;
        // In a real server, we'd check for Content-Length/Transfer-Encoding
        // For this example, we assume non-body requests (e.g., GET)
        req_co->state = DONE;
        return 1;
      }

      // Parse the header key/value pair
      if (parse_header_field(req_co) == -1)
        return -1;
    }

    // Reset line buffer for the next line
    req_co->line_len = 0;
    req_co->line_buf[0] = '\0';
  }

  // If we exit the loop, return the state
  if (req_co->state == DONE)
    return 1;
  if (req_co->state == ERROR)
    return -1;
  return 0; // Still in progress (e.g., BODY_START state)
}

/**
 * @brief Parses the METHOD, URI, and VERSION from the first line.
 * @return 0 on success, -1 on error.
 */
static int parse_request_line(Request *req_co) {
  char *line = req_co->line_buf;

  // 1. Find METHOD (token 1: ends at first space)
  char *p1 = strchr(line, ' ');
  if (p1 == NULL) {
    fprintf(stderr, "Error: Malformed Request Line (missing space 1).\n");
    req_co->state = ERROR;
    return -1;
  }
  *p1 = '\0'; // Null-terminate method
  req_co->method = safe_strdup(line);

  // 2. Find URI (token 2: ends at second space)
  char *p2 = strchr(p1 + 1, ' ');
  if (p2 == NULL) {
    fprintf(stderr, "Error: Malformed Request Line (missing space 2).\n");
    req_co->state = ERROR;
    return -1;
  }
  *p2 = '\0'; // Null-terminate URI
  req_co->uri = safe_strdup(p1 + 1);

  // 3. VERSION (token 3: to the end)
  char *version = p2 + 1;
  // Basic validation: Check for "HTTP/"
  if (strncmp(version, "HTTP/", 5) != 0) {
    fprintf(stderr, "Error: Invalid HTTP Version token.\n");
    req_co->state = ERROR;
    return -1;
  }
  req_co->version = safe_strdup(version);

  return 0;
}

/**
 * @brief Parses a single Header field (Key: Value).
 * @return 0 on success, -1 on error.
 */
static int parse_header_field(Request *req_co) {
  if (req_co->header_count >= MAX_HEADERS) {
    fprintf(stderr, "Error: Too many headers.\n");
    req_co->state = ERROR;
    return -1;
  }

  char *line = req_co->line_buf;

  // 1. Find the colon ':' that separates key and value
  char *colon = strchr(line, ':');
  if (colon == NULL) {
    fprintf(stderr, "Error: Malformed header field (missing ':').\n");
    req_co->state = ERROR;
    return -1;
  }

  // Null-terminate the key
  *colon = '\0';
  char *key = line;

  // 2. Find the value: Start after the colon, skip any leading whitespace
  char *value = colon + 1;
  while (*value != '\0' && isspace(*value)) {
    value++;
  }

  // 3. Remove any trailing whitespace from the value (optional but good
  // practice)
  size_t val_len = strlen(value);
  while (val_len > 0 && isspace(value[val_len - 1])) {
    value[--val_len] = '\0';
  }

  // 4. Store the key/value pair
  req_co->headers[req_co->header_count].key = safe_strdup(key);
  req_co->headers[req_co->header_count].value = safe_strdup(value);
  req_co->header_count++;

  return 0;
}
