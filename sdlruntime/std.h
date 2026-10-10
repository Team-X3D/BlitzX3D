#ifndef STD_H
#define STD_H

#define _WINSOCKAPI_
#define WIN32_LEAN_AND_MEAN
#define POINTER_64 __ptr64

#include "../config/config.h"
#include "../stdutil/stdutil.h"
#include "../bbruntime/constants.h"

#pragma warning( disable:4786 )

#define DIRECTSOUND_VERSION 0x700
#define NOMINMAX // stupid microsoft

#include <set>
#include <map>
#include <list>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>

#include <math.h>
#include <Windows.h>
#include <d3d9.h>

#endif