/*
 ============================================================================
 Name        : HTTP Server
 Author      : Gabriel Adelemoni
 Version     : 1.0
 Description : An event-driven HTTP Server written in pure C.
 License     : MIT
 ============================================================================
*/

#include "../include/server.h"

#define PORT "3094"   // Port number

int main(void)
{
  server_init(PORT);
}
