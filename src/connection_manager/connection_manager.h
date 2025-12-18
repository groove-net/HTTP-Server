#ifndef CONNECTION_MANAGER_PRIV_H
#define CONNECTION_MANAGER_PRIV_H

#include "../../include/connection_manager.h"

void cm_close_connection(Worker *w, int client_fd);
void coroutine_yield(Worker *worker, int fd, WaitType wt);
void coroutine_destroy(Worker *worker, Coroutine *co);

#endif
