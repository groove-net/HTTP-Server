#pragma once

#include "coroutine.h"

ssize_t recv_async(int, void *, size_t, int, Worker*);
ssize_t send_async(int, const void *, size_t, int, Worker*);
