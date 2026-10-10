
#ifndef EMITTER_H
#define EMITTER_H

#include "object.h"

class sdlSound;
class sdlChannel;

class Emitter : public Object{
public:
	Emitter();
	Emitter( const Emitter &t );
	~Emitter();

	//Entity interface
	Entity *clone(){ return new Emitter( *this ); }
	Emitter *getEmitter(){ return this; }

	//Object interface
	void beginRender( float tween );

	//Public interface
	sdlChannel *emitSound( sdlSound *sound );

private:
	Vector pos,vel;

	vector<sdlChannel*> channels;
};

#endif