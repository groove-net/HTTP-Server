#pragma once

#include "coroutine.h"

size_t recv_async(int socket, void *buffer, size_t length, int flags,
                  Worker *worker);
void send_async(int socket, const void *buffer, int length, int flags,
                Worker *worker);
