#pragma once

#include "coroutine.h"
#include <arpa/inet.h>
#include <netinet/in.h>

void *get_in_addr(struct sockaddr *sa);
int make_socket_nonblocking(int fd);
int create_listener_socket(const char *port, int backlog);
void close_connection(int fd, Worker *w);
