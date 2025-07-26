#pragma once

#include "coroutine.h"

ssize_t recv_async(int, void *, size_t, int, Worker*);
void send_async(int, const void *, int, int, Worker*);
