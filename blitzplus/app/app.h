#ifndef APP_H
#define APP_H

#include "../stdc.h"

#include "debugger.h"
#include "../string/string.h"

enum{
	BBAPP_READY,
	BBAPP_STARTING,
	BBAPP_RUNNING,
	BBAPP_ENDING
};

class		BBString;
class		BBDebugger;

int			bbAppState();

void		bbLogf( const char *msg,... );
void		bbError( const char *err,... );
void		bbAbortf( const char *err,... );

void		bbEnd();

#endif
