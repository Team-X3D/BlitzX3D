#include "sdl_gpu_shader_data.h"

#include <windows.h>
#include <dxcapi.h>
#include <d3d12shader.h>
#include <spirv_cross_c.h>

#include <cstring>
#include <fstream>
#include <sstream>

namespace sdlgpu {

	namespace {
		DxcCreateInstanceProc g_dxcCreate = nullptr;
		bool g_dxcTried = false;

		bool EnsureDxc(std::string& err) {
			if (g_dxcCreate) return true;
			if (g_dxcTried) {
				err = "dxcompiler.dll not available";
				return false;
			}
			g_dxcTried = true;
			HMODULE mod = LoadLibraryA("dxcompiler.dll");
			if (!mod) {
				err = "dxcompiler.dll not found";
				return false;
			}
			g_dxcCreate = (DxcCreateInstanceProc)GetProcAddress(mod, "DxcCreateInstance");
			if (!g_dxcCreate) {
				err = "DxcCreateInstance not found";
				return false;
			}
			return true;
		}

		std::wstring Widen(const char* s) {
			if (!s || !s[0]) return std::wstring();
			int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
			if (n <= 0) return std::wstring();
			std::wstring w(n - 1, 0);
			MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
			return w;
		}

		std::wstring ModuleDir() {
			wchar_t buf[MAX_PATH];
			DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
			if (!n) return L"";
			std::wstring s(buf, n);
			size_t p = s.find_last_of(L"\\/");
			return p == std::wstring::npos ? L"" : s.substr(0, p + 1);
		}

		std::wstring TempDirPath() {
			wchar_t buf[MAX_PATH];
			DWORD n = GetTempPathW(MAX_PATH, buf);
			if (!n) return L".\\";
			return std::wstring(buf, n);
		}

		std::wstring UniqueName() {
			static LONG counter = 0;
			wchar_t buf[80];
			swprintf_s(buf, L"%u_%u_%ld", GetCurrentProcessId(), GetTickCount(), (long)InterlockedIncrement(&counter));
			return buf;
		}

		bool FileExistsW(const std::wstring& p) {
			DWORD a = GetFileAttributesW(p.c_str());
			return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
		}

		bool WriteAllBytes(const std::wstring& path, const void* data, size_t size) {
			HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) return false;
			DWORD written = 0;
			BOOL ok = size == 0 ? TRUE : WriteFile(h, data, (DWORD)size, &written, nullptr);
			CloseHandle(h);
			return ok && (size == 0 || written == size);
		}

		bool ReadAllBytes(const std::wstring& path, std::vector<unsigned char>& out) {
			HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) return false;
			LARGE_INTEGER li{};
			GetFileSizeEx(h, &li);
			out.resize((size_t)li.QuadPart);
			DWORD read = 0;
			BOOL ok = out.empty() ? TRUE : ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr);
			CloseHandle(h);
			if (!ok) return false;
			out.resize(read);
			return true;
		}

		bool RunProcess(const std::wstring& exe, const std::wstring& cmdline, std::string& output) {
			SECURITY_ATTRIBUTES sa{};
			sa.nLength = sizeof(sa);
			sa.bInheritHandle = TRUE;
			std::wstring outPath = TempDirPath() + L"b3d_proc_" + UniqueName() + L".txt";
			HANDLE hOut = CreateFileW(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
			STARTUPINFOW si{};
			si.cb = sizeof(si);
			si.dwFlags = STARTF_USESTDHANDLES;
			si.hStdInput = hNul;
			si.hStdOutput = hOut;
			si.hStdError = hOut;
			PROCESS_INFORMATION pi{};
			std::vector<wchar_t> cmd(cmdline.begin(), cmdline.end());
			cmd.push_back(0);
			BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
			DWORD code = (DWORD)-1;
			if (ok) {
				WaitForSingleObject(pi.hProcess, INFINITE);
				GetExitCodeProcess(pi.hProcess, &code);
				CloseHandle(pi.hThread);
				CloseHandle(pi.hProcess);
			}
			if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
			if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
			std::vector<unsigned char> text;
			if (ReadAllBytes(outPath, text) && !text.empty()) output.assign((const char*)text.data(), text.size());
			DeleteFileW(outPath.c_str());
			return ok && code == 0;
		}
	}

	bool GXSHADERDATA_API RuntimeCompilerAvailable() {
		std::string err;
		return EnsureDxc(err);
	}

	std::string GXSHADERDATA_API ShaderAssetStem(const std::string& sourcePath) {
		size_t slash = sourcePath.find_last_of("/\\");
		size_t dot = sourcePath.find_last_of('.');
		if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return sourcePath;
		return sourcePath.substr(0, dot);
	}

	static bool CompileDxilBytes(const char* source, const char* entry, bool vertex, const char* includeDir, std::vector<unsigned char>& out, std::string& err) {
		if (!EnsureDxc(err)) return false;
		IDxcCompiler3* compiler = nullptr;
		if (FAILED(g_dxcCreate(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void**)&compiler)) || !compiler) {
			err = "Could not create DXC compiler";
			return false;
		}
		IDxcUtils* utils = nullptr;
		if (FAILED(g_dxcCreate(CLSID_DxcUtils, __uuidof(IDxcUtils), (void**)&utils)) || !utils) {
			compiler->Release();
			err = "Could not create DXC utils";
			return false;
		}
		IDxcIncludeHandler* includeHandler = nullptr;
		utils->CreateDefaultIncludeHandler(&includeHandler);

		std::wstring wentry = Widen(entry);
		std::wstring wtarget = vertex ? L"vs_6_0" : L"ps_6_0";
		std::wstring winclude = Widen(includeDir);
		std::vector<LPCWSTR> args;
		args.push_back(L"-E");
		args.push_back(wentry.c_str());
		args.push_back(L"-T");
		args.push_back(wtarget.c_str());
		if (!winclude.empty()) {
			args.push_back(L"-I");
			args.push_back(winclude.c_str());
		}

		DxcBuffer src{};
		src.Ptr = source;
		src.Size = strlen(source);
		src.Encoding = 0;

		IDxcResult* result = nullptr;
		HRESULT hr = compiler->Compile(&src, args.data(), (UINT32)args.size(), includeHandler, __uuidof(IDxcResult), (void**)&result);
		if (includeHandler) includeHandler->Release();
		compiler->Release();
		if (FAILED(hr) || !result) {
			err = "Shader compile failed";
			utils->Release();
			return false;
		}
		HRESULT status = S_OK;
		result->GetStatus(&status);
		if (FAILED(status)) {
			IDxcBlobUtf8* errors = nullptr;
			if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), (void**)&errors, nullptr)) && errors) {
				err.assign(errors->GetStringPointer(), errors->GetStringLength());
				errors->Release();
			}
			else {
				err = "Shader compile failed";
			}
			result->Release();
			utils->Release();
			return false;
		}
		IDxcBlob* blob = nullptr;
		if (FAILED(result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), (void**)&blob, nullptr)) || !blob) {
			err = "Shader object missing";
			result->Release();
			utils->Release();
			return false;
		}
		result->Release();
		out.assign((const unsigned char*)blob->GetBufferPointer(), (const unsigned char*)blob->GetBufferPointer() + blob->GetBufferSize());
		blob->Release();
		utils->Release();
		return true;
	}

	static bool CompileSpirvBytes(const char* source, const char* sourcePath, const char* entry, bool vertex, const char* includeDir, std::vector<unsigned char>& out, std::string& err) {
		std::wstring exe = ModuleDir() + L"dxc64\\dxc.exe";
		if (!FileExistsW(exe)) {
			err = "dxc64\\dxc.exe not found (needed for Vulkan shaders)";
			return false;
		}
		std::wstring srcPath;
		std::wstring tmpSrc;
		if (sourcePath && sourcePath[0]) {
			srcPath = Widen(sourcePath);
		}
		else {
			tmpSrc = TempDirPath() + L"b3d_shader_" + UniqueName() + L".hlsl";
			if (!WriteAllBytes(tmpSrc, source, strlen(source))) {
				err = "Unable to write temporary shader source";
				return false;
			}
			srcPath = tmpSrc;
		}
		std::wstring outPath = TempDirPath() + L"b3d_shader_" + UniqueName() + L".spv";
		std::wstring wtarget = vertex ? L"vs_6_0" : L"ps_6_0";
		std::wstring cmd = L"\"" + exe + L"\" -spirv -E \"" + Widen(entry) + L"\" -T \"" + wtarget + L"\"";
		if (includeDir && includeDir[0]) cmd += L" -I \"" + Widen(includeDir) + L"\"";
		cmd += L" -Fo \"" + outPath + L"\" \"" + srcPath + L"\"";
		std::string errText;
		bool ok = RunProcess(exe, cmd, errText);
		if (!tmpSrc.empty()) DeleteFileW(tmpSrc.c_str());
		if (ok) ok = ReadAllBytes(outPath, out);
		DeleteFileW(outPath.c_str());
		if (!ok || out.empty()) {
			err = errText.empty() ? "SPIR-V compile failed" : errText;
			return false;
		}
		return true;
	}

	static void ReflectDxilBytes(const std::vector<unsigned char>& dxil, bool vertex, ShaderStageAsset& stage) {
		std::string ensureErr;
		if (!EnsureDxc(ensureErr)) return;
		IDxcUtils* utils = nullptr;
		if (FAILED(g_dxcCreate(CLSID_DxcUtils, __uuidof(IDxcUtils), (void**)&utils)) || !utils) return;
		DxcBuffer buf{};
		buf.Ptr = dxil.data();
		buf.Size = dxil.size();
		buf.Encoding = 0;
		ID3D12ShaderReflection* refl = nullptr;
		if (SUCCEEDED(utils->CreateReflection(&buf, __uuidof(ID3D12ShaderReflection), (void**)&refl)) && refl) {
			D3D12_SHADER_DESC sd{};
			refl->GetDesc(&sd);
			for (UINT i = 0; i < sd.ConstantBuffers; ++i) {
				ID3D12ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(i);
				if (!cb) continue;
				D3D12_SHADER_BUFFER_DESC cbd{};
				if (FAILED(cb->GetDesc(&cbd)) || cbd.Type != D3D_CT_CBUFFER) continue;
				unsigned binding = 0;
				bool found = false;
				for (UINT r = 0; r < sd.BoundResources; ++r) {
					D3D12_SHADER_INPUT_BIND_DESC bd{};
					if (FAILED(refl->GetResourceBindingDesc(r, &bd))) continue;
					if (bd.Type == D3D_SIT_CBUFFER && bd.Name && cbd.Name && strcmp(bd.Name, cbd.Name) == 0) {
						binding = bd.BindPoint;
						found = true;
						break;
					}
				}
				if (!found) continue;
				if (vertex && binding == 0) continue;
				ShaderParamBuffer pb;
				pb.binding = binding;
				pb.fragment = !vertex;
				pb.dataSize = cbd.Size;
				for (UINT v = 0; v < cbd.Variables; ++v) {
					ID3D12ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
					if (!var) continue;
					D3D12_SHADER_VARIABLE_DESC vd{};
					if (FAILED(var->GetDesc(&vd)) || !vd.Name) continue;
					D3D12_SHADER_TYPE_DESC td{};
					ID3D12ShaderReflectionType* t = var->GetType();
					if (t) t->GetDesc(&td);
					ShaderParam p;
					p.offset = vd.StartOffset;
					p.size = vd.Size;
					p.vec = td.Rows > 1 ? td.Rows : td.Columns;
					if (p.vec == 0) p.vec = 1;
					p.base = td.Type;
					pb.params[vd.Name] = p;
				}
				stage.ubos.push_back(std::move(pb));
			}
			for (UINT i = 0; i < sd.BoundResources; ++i) {
				D3D12_SHADER_INPUT_BIND_DESC bd{};
				if (FAILED(refl->GetResourceBindingDesc(i, &bd))) continue;
				switch (bd.Type) {
					case D3D_SIT_CBUFFER: stage.uniformCount++; break;
					case D3D_SIT_TEXTURE:
						stage.samplerCount++;
						if (bd.Name && bd.Name[0]) stage.textures[bd.Name] = bd.BindPoint;
						break;
					case D3D_SIT_STRUCTURED:
					case D3D_SIT_BYTEADDRESS: stage.storageBufferCount++; break;
					case D3D_SIT_UAV_RWTYPED:
					case D3D_SIT_UAV_RWSTRUCTURED:
					case D3D_SIT_UAV_RWBYTEADDRESS: stage.storageTextureCount++; break;
					default: break;
				}
			}
			refl->Release();
		}
		utils->Release();
	}

	static bool ReflectSpirvBytes(const std::vector<unsigned char>& spv, bool vertex, ShaderStageAsset& stage) {
		spvc_context ctx = nullptr;
		if (spvc_context_create(&ctx) < 0) return false;
		spvc_parsed_ir ir = nullptr;
		if (spvc_context_parse_spirv(ctx, (const SpvId*)spv.data(), spv.size() / sizeof(SpvId), &ir) < 0) {
			spvc_context_destroy(ctx);
			return false;
		}
		spvc_compiler comp = nullptr;
		if (spvc_context_create_compiler(ctx, SPVC_BACKEND_NONE, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &comp) < 0) {
			spvc_context_destroy(ctx);
			return false;
		}
		spvc_resources res;
		if (spvc_compiler_create_shader_resources(comp, &res) < 0) {
			spvc_context_destroy(ctx);
			return false;
		}
		const spvc_reflected_resource* list = nullptr;
		size_t n = 0;
		if (spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, &list, &n) == 0) {
			stage.uniformCount = (unsigned)n;
			for (size_t i = 0; i < n; ++i) {
				unsigned binding = spvc_compiler_get_decoration(comp, list[i].id, SpvDecorationBinding);
				if (vertex && binding == 0) continue;
				ShaderParamBuffer pb;
				pb.binding = binding;
				pb.fragment = !vertex;
				spvc_type type = spvc_compiler_get_type_handle(comp, list[i].base_type_id);
				size_t structSize = 0;
				spvc_compiler_get_declared_struct_size(comp, type, &structSize);
				pb.dataSize = (unsigned)structSize;
				unsigned members = spvc_type_get_num_member_types(type);
				for (unsigned m = 0; m < members; ++m) {
					const char* name = spvc_compiler_get_member_name(comp, list[i].base_type_id, m);
					if (!name || !name[0]) continue;
					size_t msize = 0;
					spvc_compiler_get_declared_struct_member_size(comp, type, m, &msize);
					spvc_type mt = spvc_compiler_get_type_handle(comp, spvc_type_get_member_type(type, m));
					ShaderParam p;
					p.offset = spvc_compiler_get_member_decoration(comp, list[i].base_type_id, m, SpvDecorationOffset);
					p.size = (unsigned)msize;
					p.vec = spvc_type_get_vector_size(mt);
					if (p.vec == 0) p.vec = 1;
					p.base = spvc_type_get_basetype(mt);
					pb.params[name] = p;
				}
				stage.ubos.push_back(std::move(pb));
			}
		}
		const spvc_reflected_resource* imgs = nullptr;
		size_t ni = 0;
		if (spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_SEPARATE_IMAGE, &imgs, &ni) == 0) {
			for (size_t i = 0; i < ni; ++i) {
				unsigned binding = spvc_compiler_get_decoration(comp, imgs[i].id, SpvDecorationBinding);
				if (imgs[i].name && imgs[i].name[0]) stage.textures[imgs[i].name] = binding;
				if (binding + 1 > stage.samplerCount) stage.samplerCount = binding + 1;
			}
		}
		const spvc_reflected_resource* simgs = nullptr;
		size_t nsi = 0;
		if (spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE, &simgs, &nsi) == 0) {
			for (size_t i = 0; i < nsi; ++i) {
				unsigned binding = spvc_compiler_get_decoration(comp, simgs[i].id, SpvDecorationBinding);
				if (simgs[i].name && simgs[i].name[0]) stage.textures[simgs[i].name] = binding;
				if (binding + 1 > stage.samplerCount) stage.samplerCount = binding + 1;
			}
		}
		const spvc_reflected_resource* smps = nullptr;
		size_t ns = 0;
		if (spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_SEPARATE_SAMPLERS, &smps, &ns) == 0) {
			if (ns > stage.samplerCount) stage.samplerCount = (unsigned)ns;
		}
		const spvc_reflected_resource* sb = nullptr;
		size_t nsb = 0;
		if (spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_STORAGE_BUFFER, &sb, &nsb) == 0) stage.storageBufferCount = (unsigned)nsb;
		const spvc_reflected_resource* st = nullptr;
		size_t nst = 0;
		if (spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_STORAGE_IMAGE, &st, &nst) == 0) stage.storageTextureCount = (unsigned)nst;
		spvc_context_destroy(ctx);
		return true;
	}

	static bool CompileStage(const char* source, const char* sourcePath, const char* entry, bool vertex, const char* includeDir, bool wantDxil, bool wantSpirv, ShaderStageAsset& stage, std::string& err) {
		bool any = false;
		std::string dxilErr, spirvErr;
		if (wantDxil) {
			if (CompileDxilBytes(source, entry, vertex, includeDir, stage.dxil, dxilErr)) any = true;
		}
		if (wantSpirv) {
			if (CompileSpirvBytes(source, sourcePath, entry, vertex, includeDir, stage.spirv, spirvErr)) any = true;
		}
		if (!any) {
			err = !dxilErr.empty() ? dxilErr : spirvErr;
			return false;
		}
		if (!stage.spirv.empty()) {
			if (ReflectSpirvBytes(stage.spirv, vertex, stage)) return true;
		}
		if (!stage.dxil.empty()) {
			ReflectDxilBytes(stage.dxil, vertex, stage);
			return true;
		}
		return false;
	}

	bool GXSHADERDATA_API CompileShader(const char* source, const char* sourcePath, const char* vsEntry, const char* psEntry, const char* includeDir, bool wantDxil, bool wantSpirv, ShaderAsset& out, std::string& err) {
		if (!source || !psEntry || !psEntry[0]) {
			err = "No pixel entry point";
			return false;
		}
		if (!CompileStage(source, sourcePath, psEntry, false, includeDir, wantDxil, wantSpirv, out.ps, err)) return false;
		if (vsEntry && vsEntry[0] && strstr(source, vsEntry)) {
			out.hasVS = true;
			if (!CompileStage(source, sourcePath, vsEntry, true, includeDir, wantDxil, wantSpirv, out.vs, err)) return false;
		}
		return true;
	}

	static void WriteStageRefl(std::ostream& os, const char* tag, const ShaderStageAsset& st) {
		os << tag << " " << st.samplerCount << " " << st.uniformCount << " " << st.storageBufferCount << " " << st.storageTextureCount << "\n";
		for (const ShaderParamBuffer& b : st.ubos) {
			os << "ubo " << b.binding << " " << b.dataSize << " " << b.params.size() << "\n";
			for (const auto& kv : b.params) {
				os << "m " << kv.second.offset << " " << kv.second.size << " " << kv.second.vec << " " << kv.second.base << " " << kv.first << "\n";
			}
		}
		for (const auto& kv : st.textures) {
			os << "tex " << kv.second << " " << kv.first << "\n";
		}
	}

	bool GXSHADERDATA_API SaveShaderAsset(const std::string& stem, const ShaderAsset& asset, std::string& err) {
		std::ostringstream refl;
		refl << "b3dshader 1\n";
		WriteStageRefl(refl, "ps", asset.ps);
		if (asset.hasVS) WriteStageRefl(refl, "vs", asset.vs);

		std::ofstream rf(stem + ".refl", std::ios::binary | std::ios::trunc);
		if (!rf) {
			err = "Unable to write reflection file";
			return false;
		}
		std::string text = refl.str();
		rf.write(text.data(), (std::streamsize)text.size());
		rf.close();

		auto saveBlob = [&](const std::string& path, const std::vector<unsigned char>& bytes) -> bool {
			if (bytes.empty()) return true;
			std::ofstream f(path, std::ios::binary | std::ios::trunc);
			if (!f) return false;
			f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
			return (bool)f;
		};
		if (!saveBlob(stem + ".ps.dxil", asset.ps.dxil)) { err = "Unable to write ps dxil"; return false; }
		if (!saveBlob(stem + ".ps.spv", asset.ps.spirv)) { err = "Unable to write ps spv"; return false; }
		if (asset.hasVS) {
			if (!saveBlob(stem + ".vs.dxil", asset.vs.dxil)) { err = "Unable to write vs dxil"; return false; }
			if (!saveBlob(stem + ".vs.spv", asset.vs.spirv)) { err = "Unable to write vs spv"; return false; }
		}
		return true;
	}

	bool GXSHADERDATA_API LoadShaderAsset(const std::string& stem, ShaderAsset& out, std::string& err) {
		std::ifstream rf(stem + ".refl", std::ios::binary);
		if (!rf) {
			err = "Shader asset not found";
			return false;
		}
		std::ostringstream ss;
		ss << rf.rdbuf();
		std::string text = ss.str();
		std::istringstream in(text);
		std::string line;
		if (!std::getline(in, line) || line.rfind("b3dshader", 0) != 0) {
			err = "Invalid shader asset";
			return false;
		}
		ShaderStageAsset* current = nullptr;
		while (std::getline(in, line)) {
			std::istringstream ls(line);
			std::string tag;
			ls >> tag;
			if (tag == "ps" || tag == "vs") {
				if (tag == "vs") {
					out.hasVS = true;
					current = &out.vs;
				}
				else {
					current = &out.ps;
				}
				if (!(ls >> current->samplerCount >> current->uniformCount >> current->storageBufferCount >> current->storageTextureCount)) {
					err = "Malformed shader reflection";
					return false;
				}
				continue;
			}
			if (!current) continue;
			if (tag == "ubo") {
				ShaderParamBuffer pb;
				unsigned members = 0;
				ls >> pb.binding >> pb.dataSize >> members;
				current->ubos.push_back(std::move(pb));
			}
			else if (tag == "m") {
				if (current->ubos.empty()) continue;
				ShaderParam p;
				std::string name;
				ls >> p.offset >> p.size >> p.vec >> p.base >> name;
				current->ubos.back().params[name] = p;
			}
			else if (tag == "tex") {
				std::string name;
				unsigned binding = 0;
				ls >> binding >> name;
				current->textures[name] = binding;
			}
		}
		auto loadBlob = [&](const std::string& path, std::vector<unsigned char>& bytes) {
			if (!ReadAllBytes(Widen(path.c_str()), bytes)) bytes.clear();
		};
		loadBlob(stem + ".ps.dxil", out.ps.dxil);
		loadBlob(stem + ".ps.spv", out.ps.spirv);
		if (out.hasVS) {
			loadBlob(stem + ".vs.dxil", out.vs.dxil);
			loadBlob(stem + ".vs.spv", out.vs.spirv);
		}
		return true;
	}

	bool GXSHADERDATA_API ShaderAssetExists(const std::string& stem) {
		std::ifstream rf(stem + ".refl", std::ios::binary);
		return (bool)rf;
	}

}
