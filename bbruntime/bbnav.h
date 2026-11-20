#ifndef BBNAV_H
#define BBNAV_H

#include "std.h"

bool nav_create();
bool nav_destroy();
void nav_link(void (*rtSym)(const char* sym, void* pc));

#endif
