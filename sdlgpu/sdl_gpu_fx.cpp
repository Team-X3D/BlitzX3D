#include "sdl_gpu_fx.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>

namespace {

	using std::string;
	using std::vector;

	bool IsIdentCh(char c) {
		unsigned char u = (unsigned char)c;
		return std::isalnum(u) || c == '_';
	}

	bool IsIdentStart(char c) {
		return std::isalpha((unsigned char)c) || c == '_';
	}

	string Lower(const string& s) {
		string r = s;
		for (char& c : r) c = (char)std::tolower((unsigned char)c);
		return r;
	}

	string Upper(const string& s) {
		string r = s;
		for (char& c : r) c = (char)std::toupper((unsigned char)c);
		return r;
	}

	string Trim(const string& s) {
		size_t b = 0, e = s.size();
		while (b < e && std::isspace((unsigned char)s[b])) ++b;
		while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
		return s.substr(b, e - b);
	}

	string MakeMask(const string& s) {
		string m(s.size(), ' ');
		size_t i = 0;
		while (i < s.size()) {
			char c = s[i];
			if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
				while (i < s.size() && s[i] != '\n') { m[i] = ' '; ++i; }
				continue;
			}
			if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
				m[i] = ' '; m[i + 1] = ' '; i += 2;
				while (i < s.size()) {
					if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '/') { m[i] = ' '; m[i + 1] = ' '; i += 2; break; }
					m[i] = s[i] == '\n' ? '\n' : ' ';
					++i;
				}
				continue;
			}
			if (c == '"' || c == '\'') {
				char q = c; m[i] = ' '; ++i;
				while (i < s.size()) {
					if (s[i] == '\\' && i + 1 < s.size()) { m[i] = ' '; m[i + 1] = ' '; i += 2; continue; }
					bool done = s[i] == q;
					m[i] = (!done && s[i] == '\n') ? '\n' : ' ';
					++i;
					if (done) break;
					if (i > 0 && m[i - 1] == '\n') break;
				}
				continue;
			}
			m[i] = c;
			++i;
		}
		return m;
	}

	size_t FindWord(const string& mask, const string& word, size_t from) {
		if (word.empty()) return string::npos;
		size_t n = word.size();
		size_t p = from;
		while ((p = mask.find(word, p)) != string::npos) {
			bool before = p == 0 || !IsIdentCh(mask[p - 1]);
			bool after = p + n >= mask.size() || !IsIdentCh(mask[p + n]);
			if (before && after) return p;
			p += n;
		}
		return string::npos;
	}

	size_t SkipSpace(const string& mask, size_t p) {
		while (p < mask.size() && std::isspace((unsigned char)mask[p])) ++p;
		return p;
	}

	bool ReadIdent(const string& mask, size_t& p, string& out) {
		if (p >= mask.size() || !IsIdentStart(mask[p])) return false;
		size_t b = p;
		while (p < mask.size() && IsIdentCh(mask[p])) ++p;
		out = mask.substr(b, p - b);
		return true;
	}

	bool MatchPair(const string& mask, size_t open, char oc, char cc, size_t& closeOut) {
		if (open >= mask.size() || mask[open] != oc) return false;
		int depth = 0;
		for (size_t i = open; i < mask.size(); ++i) {
			char c = mask[i];
			if (c == oc) ++depth;
			else if (c == cc) {
				if (--depth == 0) { closeOut = i; return true; }
			}
		}
		return false;
	}

	struct Edit {
		size_t start = 0;
		size_t end = 0;
		string text;
	};

	string ApplyEdits(const string& src, vector<Edit> edits) {
		std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.start < b.start; });
		string out;
		out.reserve(src.size());
		size_t at = 0;
		for (const Edit& e : edits) {
			if (e.start < at || e.end < e.start || e.end > src.size()) continue;
			out.append(src, at, e.start - at);
			out += e.text;
			at = e.end;
		}
		out.append(src, at, string::npos);
		return out;
	}

	struct Member {
		string type;
		string name;
		string semantic;
	};

	using StructMap = std::map<string, vector<Member>>;

	string StripModifiers(string type) {
		static const char* kMods[] = { "const", "uniform", "static", "extern", "in", "out", "inout",
			"row_major", "column_major", "precise", "nointerpolation", "linear", "centroid", "sample", "noperspective" };
		string t = Trim(type);
		bool changed = true;
		while (changed) {
			changed = false;
			for (const char* m : kMods) {
				string mm = m;
				if (t.rfind(mm, 0) == 0 && t.size() > mm.size() && std::isspace((unsigned char)t[mm.size()])) {
					t = Trim(t.substr(mm.size()));
					changed = true;
				}
			}
		}
		return t;
	}

	bool ParseDecl(const string& src, const string& mask, size_t b, size_t e, Member& out) {
		size_t colon = string::npos;
		int depth = 0;
		for (size_t i = b; i < e; ++i) {
			char c = mask[i];
			if (c == '(' || c == '[') ++depth;
			else if (c == ')' || c == ']') --depth;
			else if (c == ':' && depth == 0) { colon = i; break; }
		}
		size_t nameEnd = (colon == string::npos) ? e : colon;
		while (nameEnd > b && std::isspace((unsigned char)mask[nameEnd - 1])) --nameEnd;
		if (nameEnd > b && mask[nameEnd - 1] == ']') {
			int d = 0;
			size_t i = nameEnd;
			while (i > b) {
				--i;
				if (mask[i] == ']') ++d;
				else if (mask[i] == '[') { if (--d == 0) { nameEnd = i; break; } }
			}
			while (nameEnd > b && std::isspace((unsigned char)mask[nameEnd - 1])) --nameEnd;
		}
		size_t id = nameEnd;
		while (id > b && IsIdentCh(mask[id - 1])) --id;
		if (id >= nameEnd) return false;
		out.name = src.substr(id, nameEnd - id);
		out.type = StripModifiers(Trim(src.substr(b, id - b)));
		out.semantic.clear();
		if (colon != string::npos) {
			size_t p = SkipSpace(mask, colon + 1);
			string sem;
			if (ReadIdent(mask, p, sem)) out.semantic = Upper(sem);
		}
		return !out.type.empty() && !out.name.empty();
	}

	StructMap ParseStructs(const string& src) {
		StructMap out;
		string mask = MakeMask(src);
		size_t p = 0;
		while (true) {
			size_t kw = FindWord(mask, "struct", p);
			if (kw == string::npos) break;
			size_t q = SkipSpace(mask, kw + 6);
			string name;
			if (!ReadIdent(mask, q, name)) { p = kw + 6; continue; }
			q = SkipSpace(mask, q);
			if (q >= mask.size() || mask[q] != '{') { p = kw + 6; continue; }
			size_t close = 0;
			if (!MatchPair(mask, q, '{', '}', close)) break;
			vector<Member> members;
			int depth = 0;
			size_t stmt = q + 1;
			for (size_t i = q + 1; i <= close; ++i) {
				char c = i < close ? mask[i] : ';';
				if (c == '{' || c == '(' || c == '[') ++depth;
				else if (c == '}' || c == ')' || c == ']') --depth;
				else if (c == ';' && depth == 0) {
					Member m;
					if (ParseDecl(src, mask, stmt, i, m)) members.push_back(m);
					stmt = i + 1;
				}
			}
			out[name] = members;
			p = close + 1;
		}
		return out;
	}

	string FindCompileEntry(const string& mask, size_t b, size_t e, const char* shaderKw, const char* prefix) {
		size_t p = b;
		while (p < e) {
			size_t kw = FindWord(mask, shaderKw, p);
			if (kw == string::npos || kw >= e) return string();
			size_t cp = FindWord(mask, "compile", kw + std::strlen(shaderKw));
			if (cp == string::npos || cp >= e) { p = kw + std::strlen(shaderKw); continue; }
			size_t q = SkipSpace(mask, cp + 7);
			string target, fn;
			if (!ReadIdent(mask, q, target)) { p = cp + 7; continue; }
			q = SkipSpace(mask, q);
			if (!ReadIdent(mask, q, fn)) { p = cp + 7; continue; }
			if (target.rfind(prefix, 0) == 0) return fn;
			p = q;
		}
		return string();
	}

	struct TechniqueInfo {
		string vs;
		string ps;
		vector<Edit> removals;
		bool found = false;
	};

	void ScanTechniques(const string& src, TechniqueInfo& ti) {
		string mask = MakeMask(src);
		size_t p = 0;
		while (true) {
			size_t kw = FindWord(mask, "technique", p);
			if (kw == string::npos) break;
			ti.found = true;
			size_t q = SkipSpace(mask, kw + 9);
			string name;
			ReadIdent(mask, q, name);
			q = SkipSpace(mask, q);
			if (q < mask.size() && mask[q] == '<') {
				size_t close = 0;
				if (MatchPair(mask, q, '<', '>', close)) q = close + 1;
			}
			q = SkipSpace(mask, q);
			if (q >= mask.size() || mask[q] != '{') { p = kw + 9; continue; }
			size_t close = 0;
			if (!MatchPair(mask, q, '{', '}', close)) break;
			if (ti.vs.empty()) ti.vs = FindCompileEntry(mask, q, close, "VertexShader", "vs_");
			if (ti.ps.empty()) ti.ps = FindCompileEntry(mask, q, close, "PixelShader", "ps_");
			ti.removals.push_back({ kw, close + 1, "\n" });
			p = close + 1;
		}
	}

	struct SamplerParse {
		string name;
		string texture;
		bool cube = false;
		size_t start = 0;
		size_t end = 0;
	};

	bool ScanSamplerAt(const string& src, const string& mask, size_t kw, const string& type, SamplerParse& sp) {
		size_t p = SkipSpace(mask, kw + type.size());
		string name;
		if (!ReadIdent(mask, p, name)) return false;
		size_t q = p;
		int depth = 0;
		while (q < mask.size()) {
			char c = mask[q];
			if (c == '{') ++depth;
			else if (c == '}') { if (depth == 0) break; --depth; }
			else if (c == ';' && depth == 0) break;
			++q;
		}
		if (q >= mask.size()) return false;
		string tex;
		size_t t = FindWord(mask, "Texture", p);
		if (t != string::npos && t < q) {
			size_t r = SkipSpace(mask, t + 7);
			if (r < q && mask[r] == '=') {
				r = SkipSpace(mask, r + 1);
				if (r < q && mask[r] == '<') {
					size_t close = 0;
					if (MatchPair(mask, r, '<', '>', close) && close < q) {
						size_t idp = SkipSpace(mask, r + 1);
						ReadIdent(mask, idp, tex);
					}
				}
			}
		}
		sp.name = name;
		sp.texture = tex;
		sp.cube = (type == "samplerCUBE");
		sp.start = kw;
		sp.end = q + 1;
		(void)src;
		return true;
	}

	void DumpPhase(const char* tag, const string& s) {
		const char* tmp = std::getenv("B3D_FX_DUMP");
		if (!tmp || !tmp[0]) return;
		string dir = tmp;
		if (!dir.empty() && dir.back() != "\\"[0] && dir.back() != "/"[0]) dir += "\\";
		std::ofstream f(dir + string("fxphase_") + tag + ".hlsl", std::ios::binary | std::ios::trunc);
		f.write(s.data(), (std::streamsize)s.size());
	}

	string ConvertSamplers(const string& src, vector<sdlgpu::fx::SamplerInfo>& samplers) {
		static const char* kTypes[] = { "samplerCUBE", "sampler2D", "sampler3D", "sampler1D", "sampler" };
		string work = src;
		vector<Edit> edits;
		string mask = MakeMask(work);
		size_t p = 0;
		while (true) {
			size_t best = string::npos;
			const char* type = nullptr;
			for (const char* t : kTypes) {
				size_t q = FindWord(mask, t, p);
				if (q != string::npos && (best == string::npos || q < best)) { best = q; type = t; }
			}
			if (best == string::npos) break;
			SamplerParse sp;
			if (!ScanSamplerAt(work, mask, best, type, sp)) { p = best + std::strlen(type); continue; }
			unsigned idx = (unsigned)samplers.size();
			sdlgpu::fx::SamplerInfo si;
			si.name = sp.name;
			si.texture = sp.texture;
			si.stage = idx;
			si.cube = sp.cube;
			samplers.push_back(si);
			string rep = "#if defined(B3D_STAGE_VS)\n";
			rep += string(sp.cube ? "TextureCube " : "Texture2D ") + sp.name + "_tex : register(t" + std::to_string(idx) + ", space0);\n";
			rep += "SamplerState " + sp.name + "_samp : register(s" + std::to_string(idx) + ", space0);\n";
			rep += "#else\n";
			rep += string(sp.cube ? "TextureCube " : "Texture2D ") + sp.name + "_tex : register(t" + std::to_string(idx) + ", space2);\n";
			rep += "SamplerState " + sp.name + "_samp : register(s" + std::to_string(idx) + ", space2);\n";
			rep += "#endif\n";
			edits.push_back({ sp.start, sp.end, rep });
			p = sp.end;
		}
		work = ApplyEdits(work, edits);

		mask = MakeMask(work);
		edits.clear();
		size_t r = 0;
		while (true) {
			size_t kw = FindWord(mask, "texture", r);
			if (kw == string::npos) break;
			size_t q = SkipSpace(mask, kw + 7);
			string name;
			if (!ReadIdent(mask, q, name)) { r = kw + 7; continue; }
			size_t s = SkipSpace(mask, q);
			if (s >= mask.size() || mask[s] != ';') { r = kw + 7; continue; }
			edits.push_back({ kw, s + 1, "" });
			r = s + 1;
		}
		return ApplyEdits(work, edits);
	}

	struct CallArg {
		size_t b = 0;
		size_t e = 0;
	};

	bool ParseCall(const string& mask, size_t open, size_t& closeOut, vector<CallArg>& args) {
		if (!MatchPair(mask, open, '(', ')', closeOut)) return false;
		size_t start = open + 1;
		int depth = 0;
		for (size_t i = open + 1; i < closeOut; ++i) {
			char c = mask[i];
			if (c == '(' || c == '[' || c == '{') ++depth;
			else if (c == ')' || c == ']' || c == '}') --depth;
			else if (c == ',' && depth == 0) {
				args.push_back({ start, i });
				start = i + 1;
			}
		}
		args.push_back({ start, closeOut });
		for (CallArg& a : args) {
			while (a.b < a.e && std::isspace((unsigned char)mask[a.b])) ++a.b;
			while (a.e > a.b && std::isspace((unsigned char)mask[a.e - 1])) --a.e;
		}
		return true;
	}

	void RewriteSampleCalls(string& work) {
		static const char* kFuncs[] = { "tex2Dlod", "tex2Dproj", "tex2Dbias", "tex2D", "texCUBE", "tex3D", "tex1D" };
		for (int pass = 0; pass < 8; ++pass) {
			string mask = MakeMask(work);
			vector<Edit> edits;
			bool changed = false;
			size_t pos = 0;
			while (pos < mask.size()) {
				size_t best = string::npos;
				const char* fn = nullptr;
				for (const char* f : kFuncs) {
					size_t q = FindWord(mask, f, pos);
					if (q != string::npos && (best == string::npos || q < best)) { best = q; fn = f; }
				}
				if (best == string::npos) break;
				size_t open = SkipSpace(mask, best + std::strlen(fn));
				if (open >= mask.size() || mask[open] != '(') { pos = best + std::strlen(fn); continue; }
				size_t close = 0;
				vector<CallArg> args;
				if (!ParseCall(mask, open, close, args) || args.size() < 2) { pos = open + 1; continue; }
				string sname = work.substr(args[0].b, args[0].e - args[0].b);
				if (sname.empty() || !IsIdentStart(sname[0])) { pos = close + 1; continue; }
				for (char ch : sname) {
					if (!IsIdentCh(ch)) { sname.clear(); break; }
				}
				if (sname.empty()) { pos = close + 1; continue; }
				string rest = work.substr(args[1].b, args[1].e - args[1].b);
				string tex = sname + "_tex";
				string sm = sname + "_samp";
				string rep;
				if (!std::strcmp(fn, "tex2Dlod")) rep = tex + ".SampleLevel(" + sm + ", (" + rest + ").xy, (" + rest + ").w)";
				else if (!std::strcmp(fn, "tex2Dproj")) rep = tex + ".Sample(" + sm + ", (" + rest + ").xy / (" + rest + ").w)";
				else if (!std::strcmp(fn, "tex2Dbias")) rep = tex + ".SampleBias(" + sm + ", (" + rest + ").xy, (" + rest + ").w)";
				else rep = tex + ".Sample(" + sm + ", " + rest + ")";
				edits.push_back({ best, close + 1, rep });
				changed = true;
				pos = close + 1;
			}
			if (!changed) break;
			work = ApplyEdits(work, edits);
		}
	}

	int ComponentCount(const string& type, string& base) {
		string t = Lower(Trim(type));
		string nospace;
		for (char c : t) if (!std::isspace((unsigned char)c)) nospace += c;
		size_t i = nospace.size();
		while (i > 0 && std::isdigit((unsigned char)nospace[i - 1])) --i;
		int n = 1;
		if (i < nospace.size()) n = std::atoi(nospace.c_str() + i);
		base = nospace.substr(0, i);
		if (base != "float" && base != "half" && base != "int" && base != "double" && base != "bool") return 0;
		if (n < 1 || n > 4) return 0;
		return n;
	}

	string VertexInputExpr(const string& type, const string& sem) {
		string base;
		int comp = ComponentCount(type, base);
		if (!comp) return string();
		const char* v[4] = { "0.0", "0.0", "0.0", "0.0" };
		if (sem == "POSITION" || sem == "POSITION0") {
			v[0] = "fxvi.pos.x"; v[1] = "fxvi.pos.y"; v[2] = "fxvi.pos.z"; v[3] = "1.0";
		}
		else if (sem == "NORMAL" || sem == "NORMAL0") {
			v[0] = "fxvi.nrm.x"; v[1] = "fxvi.nrm.y"; v[2] = "fxvi.nrm.z"; v[3] = "0.0";
		}
		else if (sem.rfind("COLOR", 0) == 0) {
			v[0] = "fxvi.col.x"; v[1] = "fxvi.col.y"; v[2] = "fxvi.col.z"; v[3] = "fxvi.col.w";
		}
		else if (sem == "TEXCOORD0") {
			v[0] = "fxvi.uv.x"; v[1] = "fxvi.uv.y";
		}
		else if (sem == "TEXCOORD1") {
			v[0] = "fxvi.uv1.x"; v[1] = "fxvi.uv1.y";
		}
		else if (sem == "BLENDINDICES0" || sem == "BLENDWEIGHT0") {
			v[0] = "0.0"; v[1] = "0.0"; v[2] = "0.0"; v[3] = "0.0";
		}
		else {
			return string();
		}
		if (comp == 1) return "(" + base + ")(" + v[0] + ")";
		string r = base + std::to_string(comp) + "(";
		for (int i = 0; i < comp; ++i) {
			if (i) r += ", ";
			r += v[i];
		}
		r += ")";
		return r;
	}

	struct Signature {
		string retType;
		bool found = false;
		size_t open = 0;
		size_t close = 0;
		size_t namePos = 0;
		vector<CallArg> params;
	};

	bool FindSignature(const string& work, const string& mask, const string& entry, Signature& sig) {
		size_t p = 0;
		while (true) {
			size_t kw = FindWord(mask, entry, p);
			if (kw == string::npos) return false;
			size_t open = SkipSpace(mask, kw + entry.size());
			if (open >= mask.size() || mask[open] != '(') { p = kw + entry.size(); continue; }
			size_t close = 0;
			if (!MatchPair(mask, open, '(', ')', close)) return false;
			sig.found = true;
			sig.open = open;
			sig.close = close;
			sig.namePos = kw;
			sig.params.clear();
			ParseCall(mask, open, close, sig.params);
			size_t b = kw;
			while (b > 0 && (mask[b - 1] == ' ' || mask[b - 1] == '\t' || mask[b - 1] == '\r')) --b;
			if (b == 0 || mask[b - 1] == '\n') {
				size_t j = b;
				if (j > 0) --j;
				while (j > 0 && (mask[j - 1] == ' ' || mask[j - 1] == '\t' || mask[j - 1] == '\r')) --j;
				b = j;
			}
			size_t typeEnd = b;
			while (b > 0 && IsIdentCh(mask[b - 1])) --b;
			sig.retType = StripModifiers(Trim(work.substr(b, typeEnd - b)));
			return true;
		}
	}

	string StripParamSemantics(const string& work, const Signature& sig) {
		string mask = MakeMask(work);
		vector<Edit> edits;
		for (const CallArg& a : sig.params) {
			if (a.e <= a.b) continue;
			int depth = 0;
			for (size_t i = a.b; i < a.e; ++i) {
				char c = mask[i];
				if (c == '(' || c == '[') ++depth;
				else if (c == ')' || c == ']') --depth;
				else if (c == ':' && depth == 0) { edits.push_back({ i, a.e, "" }); break; }
			}
		}
		return ApplyEdits(work, edits);
	}

	bool IsVectorType(const string& type) {
		string base;
		return ComponentCount(type, base) > 0;
	}

	void FixPixelOutputColor(string& work, const string& psEntry, const StructMap& structs) {
		string mask = MakeMask(work);
		Signature sig;
		if (!FindSignature(work, mask, psEntry, sig)) return;
		string ret = sig.retType;
		if (IsVectorType(ret)) {
			size_t p = SkipSpace(mask, sig.close + 1);
			if (p < mask.size() && mask[p] == ':') {
				size_t q = SkipSpace(mask, p + 1);
				string sem;
				size_t idPos = q;
				if (ReadIdent(mask, q, sem)) {
					string up = Upper(sem);
					if (up.rfind("COLOR", 0) == 0 || up == "SV_TARGET") {
						Edit e{ idPos, q, "SV_TARGET" };
						work = ApplyEdits(work, { e });
						return;
					}
				}
			}
			return;
		}
		auto it = structs.find(ret);
		if (it == structs.end()) return;
		for (const Member& m : it->second) {
			if (m.semantic.rfind("COLOR", 0) != 0) continue;
			string m2 = MakeMask(work);
			size_t p = 0;
			while (true) {
				size_t kw = FindWord(m2, m.name, p);
				if (kw == string::npos) break;
				size_t q = SkipSpace(m2, kw + m.name.size());
				if (q < m2.size() && m2[q] == ':') {
					size_t r = SkipSpace(m2, q + 1);
					string sem;
					size_t idPos = r;
					if (ReadIdent(m2, r, sem) && Upper(sem).rfind("COLOR", 0) == 0) {
						work = ApplyEdits(work, { Edit{ idPos, r, "SV_TARGET" } });
						return;
					}
				}
				p = kw + m.name.size();
			}
			return;
		}
	}

	bool BuildVertexAdapter(string& work, string& entry, const StructMap& structs, string& err) {
		string mask = MakeMask(work);
		Signature sig;
		if (!FindSignature(work, mask, entry, sig)) {
			err = "fx: vertex shader entry '" + entry + "' not found";
			return false;
		}
		string adapter = entry + "__b3d_vs";
		string retSem;
		{
			size_t p = SkipSpace(mask, sig.close + 1);
			if (p < mask.size() && mask[p] == ':') {
				size_t q = SkipSpace(mask, p + 1);
				string sem;
				if (ReadIdent(mask, q, sem)) retSem = Upper(sem);
			}
		}
		string ret = sig.retType;
		string retAnn;
		if (IsVectorType(sig.retType)) {
			if (retSem == "POSITION" || retSem == "POSITION0" || retSem.empty()) retAnn = " : SV_POSITION";
			else if (retSem == "VPOS") retAnn = " : SV_Position";
			else retAnn = " : " + retSem;
		}
		string gen;
		gen += "\nstruct FxB3DVertexIn { float3 pos : TEXCOORD0; float3 nrm : TEXCOORD1; float4 col : TEXCOORD2; float2 uv : TEXCOORD3; float2 uv1 : TEXCOORD4; };\n";
		gen += ret + " " + adapter + "(FxB3DVertexIn fxvi)" + retAnn + " {\n";

		vector<Member> params;
		for (const CallArg& a : sig.params) {
			if (a.e <= a.b) continue;
			Member m;
			if (!ParseDecl(work, mask, a.b, a.e, m)) {
				err = "fx: unsupported vertex shader parameter in '" + entry + "'";
				return false;
			}
			params.push_back(m);
		}

		if (params.size() == 1 && structs.count(params[0].type)) {
			const auto& members = structs.at(params[0].type);
			gen += "    " + params[0].type + " fxa;\n";
			for (const Member& m : members) {
				string expr = VertexInputExpr(m.type, m.semantic);
				if (expr.empty()) {
					string base;
					if (ComponentCount(m.type, base)) expr = "(" + m.type + ")0";
					else continue;
				}
				gen += "    fxa." + m.name + " = " + expr + ";\n";
			}
			gen += "    return " + entry + "(fxa);\n";
		}
		else if (params.empty()) {
			gen += "    return " + entry + "();\n";
		}
		else {
			string args;
			for (const Member& m : params) {
				string expr = VertexInputExpr(m.type, m.semantic);
				if (expr.empty()) {
					err = "fx: vertex shader parameter '" + m.name + "' uses an unsupported semantic";
					return false;
				}
				if (!args.empty()) args += ", ";
				args += expr;
			}
			gen += "    return " + entry + "(" + args + ");\n";
		}
		gen += "}\n";

		work = StripParamSemantics(work, sig);
		work += gen;
		entry = adapter;
		return true;
	}

	string GuessEntry(const string& work, bool wantVs) {
		string mask = MakeMask(work);
		static const char* kSkip[] = { "if", "for", "while", "switch", "return", "sizeof", "struct",
			"cbuffer", "technique", "pass", "sampler_state", "Texture2D", "SamplerState" };
		vector<string> names;
		size_t p = 0;
		while (p < mask.size()) {
			string id;
			size_t start = p;
			if (!ReadIdent(mask, p, id)) { ++p; continue; }
			(void)start;
			bool skip = false;
			for (const char* s : kSkip) if (id == s) skip = true;
			if (skip) continue;
			size_t q = SkipSpace(mask, p);
			if (q >= mask.size() || mask[q] != '(') continue;
			size_t r = start;
			while (r > 0 && std::isspace((unsigned char)mask[r - 1])) --r;
			if (r == 0 || !IsIdentCh(mask[r - 1])) continue;
			names.push_back(id);
		}
		const char* hints[] = { "vs_main", "vsmain", "VSMain", "vertexshader", "vertexmain", "ps_main", "psmain", "PSMain", "pixelshader", "pixelmain" };
		(void)hints;		for (const string& n : names) {
			string low = Lower(n);
			if (wantVs) {
				if (low == "vs_main" || low == "vsmain" || low == "vertexshader" || low == "vertex_main" || low == "vert_main")
					return n;
			}
			else {
				if (low == "ps_main" || low == "psmain" || low == "pixelshader" || low == "pixel_main" || low == "frag_main")
					return n;
			}
		}
		for (const string& n : names) {
			string low = Lower(n);
			bool isPs = low.find("ps") != string::npos || low.find("pixel") != string::npos || low.find("frag") != string::npos;
			bool isVs = low.find("vs") != string::npos || low.find("vertex") != string::npos || low.find("vert") != string::npos;
			if (wantVs && isVs && !isPs) return n;
			if (!wantVs && isPs && !isVs) return n;
		}
		return string();
	}

	vector<string> SplitTop(const string& s) {
		vector<string> out;
		int depth = 0;
		size_t start = 0;
		for (size_t i = 0; i < s.size(); ++i) {
			char c = s[i];
			if (c == '(' || c == '[' || c == '{' || c == '<') ++depth;
			else if (c == ')' || c == ']' || c == '}' || c == '>') --depth;
			else if (c == ',' && depth == 0) {
				out.push_back(s.substr(start, i - start));
				start = i + 1;
			}
		}
		out.push_back(s.substr(start));
		return out;
	}

	bool IsNumericTypeName(const string& t) {
		string s = Lower(Trim(t));
		size_t lt = s.find('<');
		if (lt != string::npos) s = s.substr(0, lt);
		size_t i = s.size();
		while (i > 0 && (std::isdigit((unsigned char)s[i - 1]) || s[i - 1] == 'x')) --i;
		string base = s.substr(0, i);
		static const char* kBase[] = { "float", "half", "double", "int", "uint", "bool", "dword",
			"min16float", "min10float", "min16int", "min16uint", "min12int" };
		for (const char* b : kBase) if (base == b) return true;
		return false;
	}

	bool ParseInitValues(const string& init, sdlgpu::fx::ParamDefault& d) {
		string s = Trim(init);
		while (!s.empty() && (s.back() == ';' || s.back() == ',')) s.pop_back();
		s = Trim(s);
		if (s.empty()) return false;
		string inner;
		if (s[0] == '{') {
			size_t close = s.rfind('}');
			if (close == string::npos || close < 1) return false;
			inner = s.substr(1, close - 1);
		}
		else {
			size_t open = s.find('(');
			size_t close = s.rfind(')');
			if (open != string::npos && close != string::npos && close > open) inner = s.substr(open + 1, close - open - 1);
			else inner = s;
		}
		vector<string> parts = SplitTop(inner);
		if (parts.empty() || parts.size() > 4) return false;
		float v[4] = { 0, 0, 0, 0 };
		for (size_t k = 0; k < parts.size(); ++k) {
			string p = Trim(parts[k]);
			if (p.empty()) return false;
			if (p.back() == 'f' || p.back() == 'F') p.pop_back();
			if (p == "true") { v[k] = 1.0f; continue; }
			if (p == "false") { v[k] = 0.0f; continue; }
			char* end = nullptr;
			float f = std::strtof(p.c_str(), &end);
			if (end == p.c_str() || (end && *end != 0)) return false;
			v[k] = f;
		}
		for (int k = 0; k < 4; ++k) d.value[k] = v[k];
		d.count = parts.size() > 1 ? 4 : 1;
		return true;
	}

	bool ExtractGlobals(string& work, vector<sdlgpu::fx::ParamDefault>& defaults, string& cbufferOut) {
		const string& src = work;
		string mask = MakeMask(src);
		vector<Edit> removals;
		string members;
		static const char* kMods[] = { "const", "static", "extern", "uniform", "volatile", "shared",
			"groupshared", "precise", "row_major", "column_major", "inline", "nointerpolation" };

		auto process = [&](size_t b, size_t e) {
			size_t b2 = b;
			while (b2 < e && std::isspace((unsigned char)mask[b2])) ++b2;
			if (b2 >= e) return;
			string body = src.substr(b2, e - b2 + 1);
			string mbody = mask.substr(b2, e - b2 + 1);
			string t = Trim(mbody);
			if (t.empty()) return;
			string low = Lower(t);
			static const char* kSkip[] = { "struct", "cbuffer", "tbuffer", "typedef", "namespace", "#", "sampler", "texture", "class", "interface" };
			for (const char* k : kSkip) if (low.rfind(k, 0) == 0) return;
			size_t paren = t.find('(');
			size_t eqFound = t.find('=');
			if (paren != string::npos && (eqFound == string::npos || paren < eqFound)) return;
			if (!body.empty() && body.back() == ';') body.pop_back();

			vector<string> ids;
			vector<size_t> idb, ide;
			size_t p = 0;
			while (p < mbody.size()) {
				if (IsIdentStart(mbody[p])) {
					size_t s = p;
					while (p < mbody.size() && IsIdentCh(mbody[p])) ++p;
					ids.push_back(mbody.substr(s, p - s));
					idb.push_back(s);
					ide.push_back(p);
					continue;
				}
				++p;
			}
			if (ids.empty()) return;
			size_t k = 0;
			bool isConst = false, isStatic = false;
			for (; k < ids.size(); ++k) {
				bool mod = false;
				for (const char* m : kMods) if (ids[k] == m) mod = true;
				if (!mod) break;
				if (ids[k] == "const") isConst = true;
				if (ids[k] == "static") isStatic = true;
			}
			if (k >= ids.size()) return;
			if (isConst || isStatic) return;
			string type = body.substr(idb[k], ide[k] - idb[k]);
			if (!IsNumericTypeName(type)) return;
			if (k + 1 >= ids.size()) return;

			string rest = body.substr(ide[k]);
			for (const string& rawDecl : SplitTop(rest)) {
				string dd = Trim(rawDecl);
				if (dd.empty()) continue;
				size_t q = 0;
				while (q < dd.size() && !IsIdentStart(dd[q])) ++q;
				size_t nb = q;
				while (q < dd.size() && IsIdentCh(dd[q])) ++q;
				string name = dd.substr(nb, q - nb);
				if (name.empty()) continue;
				size_t eqp = dd.find('=');
				string init;
				string arr;
				size_t br = dd.find('[', q);
				if (br != string::npos && (eqp == string::npos || br < eqp)) {
					size_t close = dd.find(']', br);
					if (close != string::npos) {
						arr = dd.substr(br, close - br + 1);
						if (eqp != string::npos && eqp < br) eqp = string::npos;
					}
				}
				members += "    " + type + " " + name + arr + ";\n";
				if (eqp != string::npos) {
					sdlgpu::fx::ParamDefault pd;
					pd.name = name;
					if (ParseInitValues(dd.substr(eqp + 1), pd)) defaults.push_back(pd);
				}
			}
			removals.push_back({ b2, e + 1, "" });
		};

		size_t i = 0;
		size_t stmt = 0;
		int depth = 0;
		bool lineStart = true;
		while (i < mask.size()) {
			char c = mask[i];
			if (lineStart && c == '#') {
				while (i < mask.size() && mask[i] != '\n') ++i;
				stmt = i + 1;
				lineStart = true;
				++i;
				continue;
			}
			lineStart = (c == '\n');
			if (c == '{') ++depth;
			else if (c == '}') {
				if (depth > 0) --depth;
				if (depth == 0) stmt = i + 1;
			}
			else if (c == ';' && depth == 0) {
				process(stmt, i);
				stmt = i + 1;
			}
			++i;
		}

		if (!members.empty()) {
			cbufferOut = "\n#if defined(B3D_STAGE_VS)\n";
			cbufferOut += "cbuffer FxB3DParams : register(b0, space1)\n{\n" + members + "};\n";
			cbufferOut += "#else\n";
			cbufferOut += "cbuffer FxB3DParams : register(b0, space3)\n{\n" + members + "};\n";
			cbufferOut += "#endif\n";
		}
		work = ApplyEdits(work, removals);
		return true;
	}

	vector<sdlgpu::fx::MatrixBinding> ScanMatrixBindings(const string& src) {
		vector<sdlgpu::fx::MatrixBinding> out;
		string mask = MakeMask(src);
		static const char* kTypes[] = { "float4x4", "half4x4", "float3x3", "float4x3", "float3x4", "matrix" };
		for (const char* t : kTypes) {
			size_t p = 0;
			size_t len = std::strlen(t);
			while (true) {
				size_t kw = FindWord(mask, t, p);
				if (kw == string::npos) break;
				size_t q = SkipSpace(mask, kw + len);
				string name;
				if (!ReadIdent(mask, q, name)) { p = kw + len; continue; }
				size_t r = SkipSpace(mask, q);
				if (r < mask.size() && mask[r] == ':') {
					size_t s = SkipSpace(mask, r + 1);
					string sem;
					if (ReadIdent(mask, s, sem)) {
						size_t t2 = SkipSpace(mask, s);
						if (t2 < mask.size() && (mask[t2] == ';' || mask[t2] == '[')) {
							bool dup = false;
							for (const auto& e : out) if (e.param == name) dup = true;
							if (!dup) out.push_back({ name, Upper(sem) });
						}
					}
				}
				p = kw + len;
			}
		}
		return out;
	}

}

namespace sdlgpu {
namespace fx {

	bool LooksLikeFx(const string& source) {
		string mask = MakeMask(source);
		if (FindWord(mask, "technique", 0) != string::npos) return true;
		if (FindWord(mask, "sampler_state", 0) != string::npos) return true;
		if (FindWord(mask, "sampler2D", 0) != string::npos) return true;
		if (FindWord(mask, "samplerCUBE", 0) != string::npos) return true;
		if (FindWord(mask, "sampler3D", 0) != string::npos) return true;
		if (FindWord(mask, "tex2D", 0) != string::npos) return true;
		if (FindWord(mask, "texCUBE", 0) != string::npos) return true;
		return false;
	}

	bool Convert(const string& source, const string& path, Program& out, string& err) {
		(void)path;
		string work = source;

		TechniqueInfo ti;
		ScanTechniques(work, ti);
		if (!ti.removals.empty()) work = ApplyEdits(work, ti.removals);

		out.samplers.clear();
		out.defaults.clear();
		work = ConvertSamplers(work, out.samplers);
		DumpPhase("1samplers", work);
		out.matrices = ScanMatrixBindings(work);
		string cbuffer;
		ExtractGlobals(work, out.defaults, cbuffer);
		DumpPhase("2globals", work);
		RewriteSampleCalls(work);
		DumpPhase("3calls", work);

		StructMap structs = ParseStructs(work);

		string vs = ti.vs;
		string ps = ti.ps;
		if (vs.empty()) vs = GuessEntry(work, true);
		if (ps.empty()) ps = GuessEntry(work, false);
		if (ps.empty()) {
			err = "fx: no pixel shader entry point found";
			return false;
		}
		out.psEntry = ps;

		DumpPhase("4structs", work);
		FixPixelOutputColor(work, ps, structs);

		out.vsEntry.clear();
		if (!vs.empty()) {
			string vsEntry = vs;
			if (!BuildVertexAdapter(work, vsEntry, structs, err)) return false;
			out.vsEntry = vsEntry;
		}

		DumpPhase("5adapter", work);

		{
			string mask = MakeMask(work);
			vector<Edit> edits;
			size_t p = 0;
			while (true) {
				size_t c = mask.find(':', p);
				if (c == string::npos) break;
				size_t q = SkipSpace(mask, c + 1);
				string id;
				size_t idPos = q;
				if (!ReadIdent(mask, q, id)) { p = c + 1; continue; }
				string up = Upper(id);
				if (up == "POSITION" || up == "POSITION0") edits.push_back({ idPos, q, "SV_POSITION" });
				else if (up == "VPOS" || up == "SV_VPOSITION") edits.push_back({ idPos, q, "SV_Position" });
				p = q;
			}
			work = ApplyEdits(work, edits);
		}

		out.source = cbuffer + work;
		return true;
	}

}
}
