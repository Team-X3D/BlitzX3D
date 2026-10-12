#include "sdl_gpu_shader_data.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static bool CompileOne(const std::filesystem::path& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		std::cerr << path.string() << ": cannot open\n";
		return false;
	}
	std::ostringstream ss;
	ss << f.rdbuf();
	std::string source = ss.str();

	std::string stem = sdlgpu::ShaderAssetStem(path.string());
	std::string dir = path.parent_path().string();
	std::string srcPath = path.string();

	sdlgpu::ShaderAsset asset;
	std::string err;
	if (!sdlgpu::CompileShader(source.c_str(), srcPath.c_str(), "VSMain", "PSMain", dir.c_str(), true, true, asset, err)) {
		std::cerr << path.string() << ":\n" << err << "\n";
		return false;
	}
	if (!sdlgpu::SaveShaderAsset(stem, asset, err)) {
		std::cerr << path.string() << ": " << err << "\n";
		return false;
	}
	bool dxil = !asset.ps.dxil.empty() || !asset.vs.dxil.empty();
	bool spirv = !asset.ps.spirv.empty() || !asset.vs.spirv.empty();
	std::cout << "compiled " << path.string() << (asset.hasVS ? " (vs+ps)" : " (ps)");
	if (!dxil) std::cout << " [no dxil]";
	if (!spirv) std::cout << " [no spv]";
	std::cout << "\n";
	return true;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		std::cout << "usage: shaderc <file.hlsl | folder> [more...]\n";
		return 1;
	}
	int failures = 0;
	for (int i = 1; i < argc; ++i) {
		std::filesystem::path p(argv[i]);
		std::error_code ec;
		if (std::filesystem::is_directory(p, ec)) {
			for (auto it = std::filesystem::recursive_directory_iterator(p, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
				if (!it->is_regular_file(ec)) continue;
				if (it->path().extension() != ".hlsl") continue;
				if (!CompileOne(it->path())) failures++;
			}
		}
		else {
			if (!CompileOne(p)) failures++;
		}
	}
	return failures ? 1 : 0;
}
