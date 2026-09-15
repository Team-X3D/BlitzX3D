#include "stdafx.h"
#include "debugtree.h"

#include <algorithm>

static std::string typeTag(Type* t) {
	if (t->intType()) return "";
	if (t->floatType()) return "#";
	if (t->stringType()) return "$";
	if (StructType* s = t->structType()) return "." + s->ident;
	if (VectorType* v = t->vectorType()) {
		std::string s = typeTag(v->elementType) + "[";
		for (int k = 0; k < (int)v->sizes.size(); ++k) {
			if (k) s += ",";
			s += itoa(v->sizes[k] - 1);
		}
		return s + "]";
	}
	if (ArrayType* a = t->arrayType()) {
		return typeTag(a->elementType) + "[]";
	}
	return "";
}

DebugTree::DebugTree() :st_nest(0) {
}

static void* safeDerefPtr(void* var) {
	__try {
		if (!var || IsBadReadPtr(var, sizeof(void*))) return 0;
		return *(void**)var;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

static bool safeCopyStr(void* str, std::string& out) {
	__try {
		if (!str || IsBadReadPtr(str, sizeof(BBStr))) return false;
		if (((BBStr*)str)->size() > 65536) return false;
		out = *(BBStr*)str;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

DebugTree::~DebugTree() {
}

bool DebugTree::readArray(void* var, Type* t, ArrayInfo& out) {
	__try {
	if (VectorType* vt = t->vectorType()) {
		void* buf = safeDerefPtr(var);
		if (!buf || IsBadReadPtr(buf, 4)) return false;
		out.isVector = true;
		out.buffer = buf;
		out.elemType = vt->elementType;
		out.sizes = vt->sizes;
		return !out.sizes.empty();
	}
	if (ArrayType* at = t->arrayType()) {
		if (!var || IsBadReadPtr(var, sizeof(BBArray))) return false;
		BBArray* arr = (BBArray*)var;
		if (!arr->data || arr->dims <= 0 || arr->dims > 8) return false;
		out.isVector = false;
		out.buffer = arr;
		out.elemType = at->elementType;
		out.sizes.resize(arr->dims);
		out.sizes[0] = arr->scales[0];
		for (int i = 1; i < arr->dims; ++i) {
			out.sizes[i] = arr->scales[i] / arr->scales[i - 1];
		}
		return true;
	}
	return false;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

std::string DebugTree::arrayTag(const ArrayInfo& ai) {
	std::string s = typeTag(ai.elemType);
	s += "[";
	for (int k = 0; k < (int)ai.sizes.size(); ++k) {
		if (k) s += ",";
		s += itoa(ai.sizes[k] - 1);
	}
	s += "]";
	return s;
}

void DebugTree::buildStructChildren(void* structVar, StructType* st, std::vector<Node>& out) {
	if (!st || !st->fields || structVar == 0) return;

	std::vector<Decl*> fields;
	fields.reserve(st->fields->size());
	for (int k = 0; k < st->fields->size(); ++k) fields.push_back(st->fields->decls[k]);
	std::sort(fields.begin(), fields.end(), [](Decl* a, Decl* b) {
		return a->name < b->name;
	});
	for (int k = 0; k < (int)fields.size(); ++k) {
		Decl* st_d = fields[k];
		void* st_var = (char*)structVar + st_d->offset;
		buildVar(st_var, st_d, st_d->name, out);
	}
}

void DebugTree::buildArrayChildren(const ArrayInfo& ai, std::vector<Node>& out) {
	int dims = (int)ai.sizes.size();
	if (dims <= 0) return;

	int total = 1;
	for (int k = 0; k < dims; ++k) total *= ai.sizes[k];
	if (total <= 0 || total > 20000) return;

	const char* base = ai.isVector ? (const char*)ai.buffer : (const char*)((BBArray*)ai.buffer)->data;
	if (!base) return;

	std::vector<int> idx(dims, 0);
	char buf[16];

	for (int flat = 0; flat < total; ++flat) {
		int rem = flat;
		for (int i = 0; i < dims; ++i) {
			idx[i] = rem % ai.sizes[i];
			rem /= ai.sizes[i];
		}

		std::string idxStr;
		for (int i = 0; i < dims; ++i) {
			sprintf(buf, "[%d]", idx[i]);
			idxStr += buf;
		}

		void* elem = (void*)(base + (ptrdiff_t)flat * 4);

		Node cn;
		buildElementNode(elem, ai.elemType, idxStr, cn);
		out.push_back(std::move(cn));
	}
}

void DebugTree::buildElementNode(void* var, Type* t, const std::string& id, Node& cn) {
	cn.id = id;
	std::string s = id;

	ArrayInfo ai;
	if (readArray(var, t, ai)) {
		s += arrayTag(ai);
		cn.label = s;
		if (st_nest < 4) {
			cn.expandable = true;
			++st_nest;
			buildArrayChildren(ai, cn.children);
			--st_nest;
		}
		return;
	}

	bool expandable = false;
	void* structVar = 0;

	s += typeTag(t);
	if (t->intType()) {
		s += "=" + itoa(*(int*)var);
	}
	else if (t->floatType()) {
		s += "=" + ftoa(*(float*)var);
	}
	else if (t->stringType()) {
		std::string val;
		if (safeCopyStr(safeDerefPtr(var), val)) s += "=\"" + val + '"';
		else s += "=\"\"";
	}
	else if (StructType* st = t->structType()) {
		void* v = safeDerefPtr(var);
		if (v) v = safeDerefPtr(v);
		if (!v) s += " (Null)";
		else {
			expandable = true;
			structVar = v;
		}
	}

	cn.label = s;
	cn.expandable = expandable;

	if (expandable && st_nest < 4) {
		++st_nest;
		buildStructChildren(structVar, t->structType(), cn.children);
		--st_nest;
	}
}

void DebugTree::buildVar(void* var, Decl* d, const std::string& name, std::vector<Node>& out) {
	Node n;
	n.id = name;
	std::string s = name;

	ConstType* ct = d->type->constType();
	if (ct) {
		Type* t = ct->valueType;
		s += typeTag(t);
		if (t->intType()) {
			s += "=" + itoa(ct->intValue);
		}
		else if (t->floatType()) {
			s += "=" + ftoa(ct->floatValue);
		}
		else if (t->stringType()) {
			s += "=\"" + ct->stringValue + '"';
		}
		n.label = s;
		out.push_back(std::move(n));
		return;
	}

	if (!var) {
		n.label = s;
		out.push_back(std::move(n));
		return;
	}

	ArrayInfo ai;
	if (readArray(var, d->type, ai)) {
		s += arrayTag(ai);
		n.label = s;
		if (st_nest < 4) {
			n.expandable = true;
			++st_nest;
			buildArrayChildren(ai, n.children);
			--st_nest;
		}
		out.push_back(std::move(n));
		return;
	}

	bool expandable = false;
	void* structVar = 0;

	s += typeTag(d->type);
	if (d->type->intType()) {
		s += "=" + itoa(*(int*)var);
	}
	else if (d->type->floatType()) {
		s += "=" + ftoa(*(float*)var);
	}
	else if (d->type->stringType()) {
		std::string val;
		if (safeCopyStr(safeDerefPtr(var), val)) s += "=\"" + val + '"';
		else s += "=\"\"";
	}
	else if (StructType* st = d->type->structType()) {
		void* v = safeDerefPtr(var);
		if (v) v = safeDerefPtr(v);
		if (!v) s += " (Null)";
		else {
			expandable = true;
			structVar = v;
		}
	}

	n.label = s;
	n.expandable = expandable;

	if (expandable && st_nest < 4) {
		++st_nest;
		buildStructChildren(structVar, d->type->structType(), n.children);
		--st_nest;
	}

	out.push_back(std::move(n));
}

ConstsTree::ConstsTree() :envron(0) {
}

void ConstsTree::reset(Environ* env) {
	envron = env;
	nodes.clear();
}

void ConstsTree::buildCache() {
	nodes.clear();
	if (!envron) return;

	std::vector<Decl*> decls;
	for (int k = 0; k < envron->decls->size(); ++k) {
		Decl* d = envron->decls->decls[k];
		if (!(d->kind & DECL_GLOBAL)) continue;
		if (d->type->constType()) decls.push_back(d);
	}
	std::sort(decls.begin(), decls.end(), [](Decl* a, Decl* b) {
		return a->name < b->name;
	});
	for (int k = 0; k < (int)decls.size(); ++k) {
		buildVar(0, decls[k], decls[k]->name, nodes);
	}
}

GlobalsTree::GlobalsTree() :module(0), envron(0) {
}

void GlobalsTree::reset(Module* mod, Environ* env) {
	module = mod;
	envron = env;
	nodes.clear();
}

void GlobalsTree::buildCache() {
	nodes.clear();
	if (!module || !envron) return;

	std::vector<Decl*> decls;
	for (int k = 0; k < envron->decls->size(); ++k) {
		Decl* d = envron->decls->decls[k];
		if (!(d->kind & (DECL_GLOBAL | DECL_ARRAY))) continue;
		if (d->type->constType()) continue;
		decls.push_back(d);
	}
	std::sort(decls.begin(), decls.end(), [](Decl* a, Decl* b) {
		return a->name < b->name;
	});

	for (int k = 0; k < (int)decls.size(); ++k) {
		Decl* d = decls[k];
		void* var = 0;
		if (d->type->arrayType()) {
			module->findSymbol(("_a" + d->name).c_str(), (int*)&var);
		}
		else {
			module->findSymbol(("_v" + d->name).c_str(), (int*)&var);
		}
		buildVar(var, d, d->name, nodes);
	}
}

LocalsTree::LocalsTree() {
}

void LocalsTree::reset(Environ* env) {
	(void)env;
	frames.clear();
	nodes.clear();
}

void LocalsTree::buildCache() {
	nodes.clear();
	if (!frames.size()) return;

	for (int n = 0; n < (int)frames.size(); ++n) {
		const Frame& f = frames[n];

		Node fn;
		fn.id = f.func + "##" + itoa(n);
		fn.label = f.func;
		fn.expandable = true;

		std::vector<Decl*> decls;
		for (int k = 0; k < f.env->decls->size(); ++k) {
			Decl* d = f.env->decls->decls[k];
			if (!(d->kind & (DECL_LOCAL | DECL_PARAM))) continue;
			if (d->name.empty() || !isalpha((unsigned char)d->name[0])) continue;
			decls.push_back(d);
		}
		std::sort(decls.begin(), decls.end(), [](Decl* a, Decl* b) {
			return a->name < b->name;
		});
		for (int k = 0; k < (int)decls.size(); ++k) {
			Decl* d = decls[k];
			buildVar((char*)f.frame + d->offset, d, d->name, fn.children);
		}

		nodes.push_back(std::move(fn));
	}
}

void LocalsTree::pushFrame(void* f, void* e, const char* func) {
	frames.push_back(Frame(f, (Environ*)e, func));
}

void LocalsTree::popFrame() {
	if (!frames.empty()) frames.pop_back();
}
