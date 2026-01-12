### What about REST APIs?

Angular and other modern web frameworks often use **REST APIs**. By restricting your static file middleware to `GET`, you clear the way for your server to handle other methods differently.

We currently have two middlewares, URL decoder and static file router. The decoder ****reformats the URL, while s**tatic files,** server files on a `GET` request if the file exists.

**We may add a final API Router middleware,** If the method is `POST/PUT/DELETE` or a `GET` not in reference to a file, check if it matches an API endpoint (e.g., `/api/login`). Before doing a f**inal fallback** If nothing matched, by sending the **404**.

**You can see how you would implement a basic "API Switchboard" so you can handle stuff like `POST` requests for things like a login or data submission alongside your Angular app.**

To make a REST API in C, you’ll usually also need JSON Parsers. Here are a few popular libraries:

- **jansson** — simple, widely used.
- [**cJSON**](https://github.com/DaveGamble/cJSON) — lightweight, C89-compatible.
- [**json-c**](https://github.com/json-c/json-c) — more full-featured.
