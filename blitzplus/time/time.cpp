#include "time.h"

#include <windows.h>
#include <time.h>

int bbMilliSecs(){
	return (int)GetTickCount();
}

void bbDelay( int ms ){
	Sleep( ms );
}

BBString* bbCurrentDate(){
	time_t t=time(0);
	struct tm *tmv=localtime(&t);
	char buf[32];
	strftime( buf,sizeof(buf),"%d %b %Y",tmv );
	return new BBString( buf );
}

BBString* bbCurrentTime(){
	time_t t=time(0);
	struct tm *tmv=localtime(&t);
	char buf[32];
	strftime( buf,sizeof(buf),"%H:%M:%S",tmv );
	return new BBString( buf );
}
