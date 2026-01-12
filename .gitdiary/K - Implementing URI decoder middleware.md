### Setting up the middleware pipeline

In the `src/request_handler/` folder, create a folder called `middlewares/` and within that folder create a file called `middleware_pipeline.c`:

```
📂 httplite/
├── 📂 src/
│   ├── 📂 request_handler/
│   │   └── 📂 middlewares/
│   │       └── 📄 middleware_pipeline.c
│   └── 📄 main.c
└── ...
```

`src / request_handler / middlewares / middleware_pipeline.c`:

```c
// Start Middleware Pipeline
void middleware_pipeline(Request *req, int keep_alive, Worker *w,
                         int client_fd) {
  // TODO: call first middleware
}
```

Our first middleware will be a URI decoder.

### URL encoding

To handle URL encoding (like `%20` for spaces or `%2B` for `+`), we need a decoder. Browsers encode any character that isn't a "safe" ASCII character to ensure the URI remains valid during transit. If your Angular app has a file named `my report.pdf`, the browser will request `my%20report.pdf`. Without a decoder, `stat()` will look for the literal string with the percent signs and return a **404**. This function decodes the URI in-place. Since a decoded string is always shorter or equal in length to the encoded one, we don't need to allocate new memory.

In the `src/request_handler/middlewares/` folder, create a file called `url_decoder_middleware.c`:

```
📂 httplite/
├── 📂 src/
│   ├── 📂 request_handler/
│   │   └── 📂 middlewares/
│   │       └── 📄 url_decoder_middleware.c
│   └── 📄 main.c
└── ...
```

`src / request_handler / middlewares / url_decoder_middleware.c`:

```c
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

  // TODO: go next middleware
}
```

This should be the entry point in the pipeline. Back in `middleware_pipeline.c` , include the `uri_decoder_middleware.c` file: 

```c
#include "uri_decoder_middleware.c"
```

Then call the `uri_decoder_middleware` function immediately:

```c
// Start Middleware Pipeline
void middleware_pipeline(Request *req, int keep_alive, Worker *w,
                         int client_fd) {
  uri_decoder_middleware(req, keep_alive, w, client_fd); // ADD THIS   
}
```

### Invoking the pipeline

Now we can invoke our middleware pipeline from our `entry` function. First, include `middlewares/middleware_pipeline.c` in our `request_handler.c` file:

```c
#include "./middlewares/middleware_pipeline.c"
```

Then, in that same file, invoke the `middleware_pipeline` function after a successfully completed parse:

```c
// ...

    if (parse_state == 1) { // Successfully parsed, begin middleware pipeline
      middleware_pipeline(&req, 0, w, client_fd); // ADD THIS
      request_cleanup(&req);
      break;
    }
    
// ...
```
