#ifndef BBANGEL_H_INCLUDED
#define BBANGEL_H_INCLUDED

#include <string>

bool angel_create();
bool angel_destroy();
void angel_link(void (*rtSym)(const char* sym, void* pc));

bool angel_is_executing();
std::string getAngelStackTrace();

#endif // BBANGEL_H_INCLUDED
