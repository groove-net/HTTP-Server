#pragma once

#define BACKLOG 10    // Max number of pending connections in the queue
#define MAX_EVENTS 64 // Max number of events returned by epoll

/*
 * This initializes the HTTP server on the specified PORT
 */
void server_init(const char *);
