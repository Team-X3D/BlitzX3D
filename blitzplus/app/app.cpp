#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

BBDebugger *bbDebugger = 0;

typedef void (*BBErrorHook)( const char *msg );
typedef void (*BBLogHook)( const char *msg );

static BBErrorHook _err_hook = 0;
static BBLogHook _log_hook = 0;
static int _app_state = BBAPP_READY;

void bbSetAppHooks( BBErrorHook err, BBLogHook log ){
	_err_hook = err;
	_log_hook = log;
}

void bbSetAppState( int state ){
	_app_state = state;
}

int bbAppState(){
	return _app_state;
}

void bbLogf( const char *msg,... ){
	char buf[1024];
	va_list args;
	va_start( args,msg );
	vsnprintf( buf,sizeof(buf),msg,args );
	va_end( args );

	if( _log_hook ){
		_log_hook( buf );
		return;
	}
	fprintf( stderr,"%s\n",buf );
	fflush( stderr );
}

void bbError( const char *err,... ){
	char buf[1024];
	va_list args;
	va_start( args,err );
	vsnprintf( buf,sizeof(buf),err,args );
	va_end( args );

	if( _err_hook ){
		_err_hook( buf );
		return;
	}
	fprintf( stderr,"%s\n",buf );
	fflush( stderr );
	exit( -1 );
}

void bbAbortf( const char *err,... ){
	char buf[1024];
	va_list args;
	va_start( args,err );
	vsnprintf( buf,sizeof(buf),err,args );
	va_end( args );

	if( _err_hook ){
		_err_hook( buf );
		return;
	}
	fprintf( stderr,"%s\n",buf );
	fflush( stderr );
	exit( -1 );
}

void bbEnd(){
	_app_state = BBAPP_ENDING;
}

void BBDebugger::debugRun(){}
void BBDebugger::debugStop(){}
void BBDebugger::debugStmt( int srcpos,const char *file ){}
void BBDebugger::debugEnter( void *frame,void *env,const char *func ){}
void BBDebugger::debugLeave(){}
void BBDebugger::debugLog( const char *msg ){}
void BBDebugger::debugMsg( const char *msg,bool serious ){}
