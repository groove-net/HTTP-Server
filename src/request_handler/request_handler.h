#ifndef REQUEST_HANDLER_PRIVATE_H
#define REQUEST_HANDLER_PRIVATE_H

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

#endif
