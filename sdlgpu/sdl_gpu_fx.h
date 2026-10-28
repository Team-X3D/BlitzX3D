#ifndef SDL_GPU_FX_H
#define SDL_GPU_FX_H

#include <string>
#include <vector>

namespace sdlgpu {
namespace fx {

	struct SamplerInfo {
		std::string name;
		std::string texture;
		unsigned stage = 0;
		bool cube = false;
	};

	struct MatrixBinding {
		std::string param;
		std::string semantic;
	};

	struct ParamDefault {
		std::string name;
		float value[4] = { 0, 0, 0, 0 };
		int count = 1;
	};

	struct Program {
		std::string source;
		std::string vsEntry;
		std::string psEntry;
		std::vector<SamplerInfo> samplers;
		std::vector<MatrixBinding> matrices;
		std::vector<ParamDefault> defaults;
	};

	bool LooksLikeFx(const std::string& source);
	bool Convert(const std::string& source, const std::string& path, Program& out, std::string& err);

}
}

#endif
