## 🚀 HTTP/1.1 Parser Implementation

Although there exists a few production grade Http parsers such as:

- [**http-parser**](https://github.com/nodejs/http-parser) — from the Node.js project.
- [**llhttp**](https://github.com/nodejs/llhttp) — successor to `http-parser`.

it is still while to build our own from scratch. Implementing a full, robust HTTP/1.1 parser from scratch is a significant undertaking, but I can provide a comprehensive, step-by-step implementation in C that correctly handles the Request Line and Headers, using the state machine pattern for partial data.

This implementation will focus on the most critical challenge: **correctly identifying and extracting tokens and lines** while buffering incoming, asynchronous data.

### Step 1: Data Structures and Constants

We define the necessary constants, the states for the state machine, and the structures to hold the request data and the current parsing context.

In the `src/request_handler/` folder, create a file called `request_handler.h`:

```
📂 httplite/
├── 📂 src/
│   ├── 📂 request_handler/
│   │   └── 📄 request_handler.h
│   └── 📄 main.c
└── ...
```

`src` / `request_handler` / `request_handler.h`:

```c
#ifndef REQUEST_HANDLER_PRIVATE_H
#define REQUEST_HANDLER_PRIVATE_H

// --- Includes ---
#include "../../include/connection_manager.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

// --- Constants ---
#define INITIAL_BUF_SIZE 1024
#define MAX_HEADERS 100
#define MAX_LINE_LEN 8192 // Max size for a single Request Line or Header

// --- State Machine Enums ---
typedef enum {
  NEW,        // Start of request
  RL_METHOD,  // Parsing method (e.g., GET)
  RL_URI,     // Parsing URI
  RL_VERSION, // Parsing version (e.g., HTTP/1.1)
  HEADERS,    // Parsing header key/value pairs
  BODY_START, // Done with headers, preparing for body/done
  DONE,       // Successfully parsed request
  ERROR       // Parsing error (bad request)
} RequestState;

// --- Data Structures ---
typedef struct {
  char *key;
  char *value;
} Header;

typedef struct Request {
  RequestState state;

  // Request Line Components (dynamically allocated)
  char *method;
  char *uri;
  char *version;

  // Header Components
  Header headers[MAX_HEADERS];
  int header_count;

  // Line/Token Buffering (for current state processing)
  char line_buf[MAX_LINE_LEN];
  int line_len; // Current length of data in line_buf

  // For overall request management (not used in this simplified parser)
  // ssize_t content_length;
} Request;

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
inline void request_init(Request *req) {
  if (!req)
    return;
  memset(req, 0, sizeof(Request));
  req->state = NEW;
}

/**
 * @brief Cleans up dynamically allocated memory in the Request object.
 */
inline void request_cleanup(Request *req) {
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

#endif
```

### Step 2: The Line Reader Helper

HTTP parsing is primarily line-based until the message body. We need a helper function to reliably extract a full line (`/r/n`) from the input buffer, handling the case where the `/r/n` might be split across two separate `read()` calls.

In the `src/request_handler/` folder, create a file called `http_parser.c`:

```
📂 httplite/
├── 📂 src/
│   ├── 📂 request_handler/
│   │   └── 📄 http_parser.c
│   └── 📄 main.c
└── ...
```

Still in `src` / `request_handler` / `http_parser.c`:

```c
#include "request_handler.h"

/**
 * @brief Appends data to the internal line buffer, looking for \r\n.
 * @param req_co The request context.
 * @param buf The incoming data buffer pointer (will be advanced).
 * @param nbytes_ptr Pointer to remaining bytes (will be decremented).
 * @return 1 if a full line (\r\n) was found, 0 if partial, -1 on error (line too long).
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
            req_co->line_len -= 2; // Line length without \r\n

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
```

### Step 3: Parsing the Request Line and Headers

This is the main state machine. It uses the `read_line` helper to get complete lines and transitions states based on the content.

Still in `src` / `request_handler` / `http_parser.c`:

```c
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
    while (remaining_bytes > 0 && 
           req_co->state != DONE && 
           req_co->state != ERROR) {
        
        // Use the line reader for states that require a full line
        if (req_co->state >= NEW && req_co->state <= HEADERS) {
            line_status = read_line(req_co, &current_buf_ptr, &remaining_bytes);
        } else {
            // For BODY/other states, logic would go here.
            // Since we only implement RL and Headers, we break if a line isn't needed.
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
            if (parse_request_line(req_co) == -1) return -1;
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
            if (parse_header_field(req_co) == -1) return -1;
            
        }
        
        // Reset line buffer for the next line
        req_co->line_len = 0;
        req_co->line_buf[0] = '\0';
    }

    // If we exit the loop, return the state
    if (req_co->state == DONE) return 1;
    if (req_co->state == ERROR) return -1;
    return 0; // Still in progress (e.g., BODY_START state)
}
```

### Step 4: Tokenization and Validation Logic

This is where the actual HTTP specification rules are applied (e.g., finding the spaces in the Request Line, finding the colon in headers).

Still in `src` / `request_handler` / `http_parser.c`:

```c
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
    
    // 3. Remove any trailing whitespace from the value (optional but good practice)
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
```

### Step 5: Testing / Demonstration (read-only)

Here is a simple `main` function to demonstrate how the parser handles data coming in chunks (partial data).

```c
void print_request(const Request *req) {
    printf("\n--- PARSING COMPLETE (State: %s) ---\n", 
           (req->state == DONE ? "DONE" : "ERROR"));
    if (req->state != DONE) return;
    
    printf("Method: %s\n", req->method);
    printf("URI:    %s\n", req->uri);
    printf("Version: %s\n", req->version);
    
    printf("Headers (%d):\n", req->header_count);
    for (int i = 0; i < req->header_count; i++) {
        printf("  > %s: %s\n", req->headers[i].key, req->headers[i].value);
    }
    printf("--------------------------------------\n");
}

int main() {
    Request req;
    request_init(&req);

    // A complete HTTP request
    const char *full_request = 
        "GET /index.html?q=test HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "User-Agent: C-Client\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
        
    // --- Test 1: Full request in one shot ---
    printf("Test 1: Full request in one buffer\n");
    int status = parse_http(&req, full_request, strlen(full_request));
    print_request(&req);
    request_cleanup(&req);
    
    // --- Test 2: Request split into multiple chunks (Partial Parsing) ---
    request_init(&req);
    printf("\nTest 2: Request split into multiple buffers\n");

    const char *chunk1 = "GET /foo";
    const char *chunk2 = "bar.htm";
    const char *chunk3 = "l HTTP/1.1\r\nHost: example.c";
    const char *chunk4 = "om\r\nUser-Agent: Test\r\n\r\n";

    printf("Parsing chunk 1...\n");
    status = parse_http(&req, chunk1, strlen(chunk1)); // Expects 0 (Partial)
    printf("  Status: %d, State: %d\n", status, req.state);

    printf("Parsing chunk 2...\n");
    status = parse_http(&req, chunk2, strlen(chunk2)); // Expects 0 (Partial)
    printf("  Status: %d, State: %d\n", status, req.state);

    printf("Parsing chunk 3 (contains \r\n)...\n");
    status = parse_http(&req, chunk3, strlen(chunk3)); // Expects 0 (Partial/Headers)
    printf("  Status: %d, State: %d\n", status, req.state);
    
    printf("Parsing chunk 4 (contains \r\n\r\n)...\n");
    status = parse_http(&req, chunk4, strlen(chunk4)); // Expects 1 (DONE)
    printf("  Status: %d, State: %d\n", status, req.state);

    print_request(&req);
    request_cleanup(&req);

    return 0;
}
```

### Improvements

This parser is a great start—it’s clean, state-driven, and handles the fundamental parts of an HTTP request. Since we’re building a high-performance coroutine server, there are a few architectural "level-ups" we can apply to make this as "lean" as your networking code.

**1. The State Machine Analysis**

Your state machine is well-defined. By using `read_line` as a precursor to the state logic, you simplify the parsing of the Request Line and Headers.

**2. "Lean" Optimization: Avoiding `strdup`**

Currently, you are calling `malloc` (via `safe_strdup`) for the method, URI, version, and every single header. In a high-concurrency server, this adds significant overhead. Since you already have the request data in a buffer (your `line_buf`), you can store **pointers** (offsets) into a persistent buffer instead of copying strings.

- If you decide to keep `line_buf`, you must keep the `strdup` because `line_buf` is overwritten every line.
- **Better Approach:** If you read the entire header block into one larger buffer, you can just set pointers to the start of each word and null-terminate them in place. This is "Zero-Copy" parsing.

**3. Missing: Request Line Validation**

HTTP/1.1 is very specific about the Request Line. Your parser currently fails if there is more than one space between the Method and URI.

- **The Fix:** Use a more robust tokenization or skip multiple spaces using `isspace()` in a loop.

**4. Header Security: "Slowloris" and Buffer Limits**

You have `MAX_HEADERS` and `MAX_LINE_LEN`, which is excellent protection. However, consider the total size of headers. A malicious client could send 16 headers that are each `2047` bytes long, forcing your `Request` struct (on the coroutine stack) to be quite large.

> Warning: Your Request struct is currently quite large because of char line_buf[MAX_LINE_LEN]. Since this struct is likely on your coroutine's stack, make sure your STACK_SIZE (64KB) can accommodate it, especially if you have multiple nested function calls.
> 

---

### Suggested Leaner Refinements

**Refinement A: Tokenization logic**

Instead of `strchr`, using a simple pointer walk is often faster and handles malformed input better.

```c
static int parse_request_line(Request *req_co) {
    char *p = req_co->line_buf;
    
    // Skip leading spaces (not strictly allowed by RFC but good for robustness)
    while (*p && isspace(*p)) p++;
    
    // Method
    char *method_start = p;
    while (*p && !isspace(*p)) p++;
    if (!*p) return -1; 
    *p++ = '\0';
    req_co->method = safe_strdup(method_start);

    while (*p && isspace(*p)) p++; // Skip spaces between tokens
    
    // URI
    char *uri_start = p;
    while (*p && !isspace(*p)) p++;
    if (!*p) return -1;
    *p++ = '\0';
    req_co->uri = safe_strdup(uri_start);

    // ... and so on
}
```

**Refinement B: Header Normalization**

HTTP headers are **case-insensitive**. It is usually helpful to convert the `key` to lowercase during parsing so your `handler` doesn't have to check for "Content-Length", "content-length", and "CONTENT-LENGTH".

---

**Summary Checklist for the Parser**

| **Feature** | **Current Status** | **Recommendation** |
| --- | --- | --- |
| **State Machine** | Solid | Keep as is; it works well with your coroutine `yield`. |
| **Memory** | Multiple `mallocs` | If performance is key, move to a single buffer and pointers. |
| **Robustness** | Basic | Add a check for "Transfer-Encoding" or "Content-Length" soon. |
| **Stack Usage** | High | Check `sizeof(Request)` vs your coroutine stack. |
