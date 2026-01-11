#include "../../../include/connection_manager.h"
#include "../request_handler.h"
#include "handle_static_file_middleware.c"

#include <ctype.h>

void uri_decoder_middleware(Request *req, int keep_alive, Worker *w,
                            int client_fd) {
  char *src = req->uri;
  char *dst = req->uri;

  while (*src) {
    if (*src == '%' && src[1] && src[2]) {
      // Hex to Int conversion (e.g., %20 -> 32)
      int hi = isdigit(src[1]) ? src[1] - '0' : toupper(src[1]) - 'A' + 10;
      int lo = isdigit(src[2]) ? src[2] - '0' : toupper(src[2]) - 'A' + 10;
      *dst++ = (char)((hi << 4) | lo);
      src += 3;
    } else if (*src == '+') {
      // Some older clients use '+' for spaces in query strings
      *dst++ = ' ';
      src++;
    } else {
      *dst++ = *src++;
    }
  }
  *dst = '\0';

  // go next middleware
  handle_static_file_middleware(req, keep_alive, w, client_fd);
}
