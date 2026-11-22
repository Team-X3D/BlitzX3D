#ifndef LOADER_FBX_H
#define LOADER_FBX_H

#include "meshloader.h"

class Loader_FBX : public MeshLoader {
public:
	MeshModel* load(const std::string& f, const Transform& conv, int hint);
};

#endif
