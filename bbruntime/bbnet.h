#ifndef BBNET_H
#define BBNET_H

#include "std.h"

bool net_create();
bool net_destroy();
void net_link(void (*rtSym)(const char* sym, void* pc));

#endif
