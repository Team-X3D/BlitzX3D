#include "std.h"
#include "nodes.h"
#include "../MultiLang/MultiLang.h"
#include <format>

std::set<std::string> Node::usedfuncs;
std::unordered_map<std::string, std::vector<OverrideFunction>> OverrideFunctionMap;

thread_local bool Ex_collecting = false;
thread_local std::vector<Ex>* Ex_errors = nullptr;

///////////////////////////////
// generic exception thrower //
///////////////////////////////
void Node::ex() {
	ex(MultiLang::internal_compiler_error);
}

void Node::ex(const std::string& e) {
	throw Ex(e, -1, "");
}

void Node::ex(const std::string& e, int pos) {
	throw Ex(e, pos, "");
}

void Node::ex(const std::string& e, int pos, const std::string& f) {
	throw Ex(e, pos, f);
}

///////////////////////////////
// Generate a local variable //
///////////////////////////////
VarNode* Node::genLocal(Environ* e, Type* ty) {
	std::string t = genLabel();
	Decl* d = e->decls->insertDecl(t, ty, DECL_LOCAL);
	return new DeclVarNode(d);
}

/////////////////////////////////////////////////
// if type is const, return const value else 0 //
/////////////////////////////////////////////////
ConstNode* Node::constValue(Type* ty) {
	ConstType* c = ty->constType();
	if(!c) return 0;
	ty = c->valueType;
	if(ty == Type::int_type) return new IntConstNode(c->intValue);
	if(ty == Type::float_type) return new FloatConstNode(c->floatValue);
	if(ty == Type::null_type) return new NullConstNode();
	return new StringConstNode(c->stringValue);
}

///////////////////////////////////////////////////////
// work out var offsets - return size of local frame //
///////////////////////////////////////////////////////
int Node::enumVars(Environ* e) {
	//calc offsets
	int p_size = 0, l_size = 0;
	for(int k = 0; k < e->decls->size(); ++k) {
		Decl* d = e->decls->decls[k];
		if(d->kind & DECL_PARAM) {
			d->offset = p_size + 20;
			p_size += 4;
		}
		else if(d->kind & DECL_LOCAL) {
			d->offset = -4 - l_size;
			l_size += 4;
		}
	}
	return l_size;
}

//////////////////////////////
// initialize all vars to 0 //
//////////////////////////////
TNode* Node::createVars(Environ* e) {
	int k;
	TNode* t = 0;
	//initialize locals
	for(k = 0; k < e->decls->size(); ++k) {
		Decl* d = e->decls->decls[k];
		if(d->kind != DECL_LOCAL) continue;
		if(d->type->vectorType()) continue;
		if(!t) t = new TNode(IR_CONST, 0, 0, 0);
		TNode* p = new TNode(IR_LOCAL, 0, 0, d->offset);
		p = new TNode(IR_MEM, p, 0);
		t = new TNode(IR_MOVE, t, p);
	}
	//initialize vectors
	for(k = 0; k < e->decls->size(); ++k) {
		Decl* d = e->decls->decls[k];
		if(d->kind == DECL_PARAM) continue;
		VectorType* v = d->type->vectorType();
		if(!v) continue;
		TNode* p = call("__bbVecAlloc", global(v->label));
		TNode* m = d->kind == DECL_GLOBAL ? global("_v" + d->name) : local(d->offset);
		p = move(p, mem(m));
		if(t) t = seq(t, p);
		else t = p;
	}
	return t;
}

////////////////////////
// release local vars //
////////////////////////
TNode* Node::deleteVars(Environ* e) {
	TNode* t = 0, * l = 0, * p, * p1, * p2;
	for(int k = 0; k < e->decls->size(); ++k) {
		Decl* d = e->decls->decls[k];
		Type* type = d->type;
		std::string func;
		if(type == Type::string_type) {
			if(d->kind == DECL_LOCAL || d->kind == DECL_PARAM) {
				func = "__bbStrRelease";
				p1 = mem(local(d->offset));
				p2 = 0;
			}
		}
		else if(type->structType()) {
			if(d->kind == DECL_LOCAL) {
				func = "__bbObjRelease";
				p1 = mem(local(d->offset));
				p2 = 0;
			}
		}
		else if(VectorType* v = type->vectorType()) {
			if(d->kind == DECL_LOCAL) {
				func = "__bbVecFree";
				p1 = mem(local(d->offset));
				p2 = global(v->label);
			}
		}
		if(!func.size()) continue;
		p = new TNode(IR_SEQ, call(func, p1, p2), 0);
		(l ? l->r : t) = p;
		l = p;
	}
	return t;
}

//////////////////////////////////////////////////////////////
// compare 2 translated operands - return 1 if true, else 0 //
//////////////////////////////////////////////////////////////
TNode* Node::compare(int op, TNode* l, TNode* r, Type* ty) {
	int n = 0;
	if(ty == Type::float_type) {
		switch(op) {
			case '=':n = IR_FSETEQ; break; case NE:n = IR_FSETNE; break;
			case '<':n = IR_FSETLT; break; case '>':n = IR_FSETGT; break;
			case LE:n = IR_FSETLE; break; case GE:n = IR_FSETGE; break;
		}
	}
	else {
		switch(op) {
			case '=':n = IR_SETEQ; break; case NE:n = IR_SETNE; break;
			case '<':n = IR_SETLT; break; case '>':n = IR_SETGT; break;
			case LE:n = IR_SETLE; break; case GE:n = IR_SETGE; break;
		}
	}
	if(ty == Type::string_type) {
		l = call("__bbStrCompare", l, r);
		r = new TNode(IR_CONST, 0, 0, 0);
	}
	else if(ty->structType()) {
		l = call("__bbObjCompare", l, r);
		r = new TNode(IR_CONST, 0, 0, 0);
	}
	return new TNode(n, l, r);
}

/////////////////////////////////
// calculate the type of a tag //
/////////////////////////////////
Type* Node::tagType(const std::string& tag, Environ* e) {
	Type* t;
	if(tag.size()) {
		t = e->findType(tag);
		if(!t) ex(std::format(MultiLang::type_not_found, tag));
	}
	else t = 0;
	return t;
}

int Node::typeSize(Type* t) {
	if(!t) return 4;
	if(t == Type::int_type || t == Type::float_type) return 4;
	return targetPtrSize;
}

////////////////////////////////
// Generate a fresh ASM label //
////////////////////////////////
std::string Node::genLabel() {
	static int cnt;
	return "_" + itoa(++cnt & 0x7fffffff);
}

//////////////////////////////////////////////////////
// create a stmt-type function call with int result //
//////////////////////////////////////////////////////
TNode* Node::call(const std::string& func, TNode* a0, TNode* a1, TNode* a2) {
	int size = 0;
	TNode* t = 0;
	if(a0) {
		t = move(a0, mem(arg(0)));
		size += 4;
		if(a1) {
			t = seq(t, move(a1, mem(arg(4))));
			size += 4;
			if(a2) {
				t = seq(t, move(a2, mem(arg(8))));
				size += 4;
			}
		}
	}
	TNode* l = new TNode(IR_GLOBAL, 0, 0, func);
	return new TNode(IR_CALL, l, t, size);
}

////////////////////////////////////////////////////////
// create a stmt-type function call with float result //
////////////////////////////////////////////////////////
TNode* Node::fcall(const std::string& func, TNode* a0, TNode* a1, TNode* a2) {
	int size = 0;
	TNode* t = 0;
	if(a0) {
		t = move(a0, mem(arg(0)));
		size += 4;
		if(a1) {
			t = seq(t, move(a1, mem(arg(4))));
			size += 4;
			if(a2) {
				t = seq(t, move(a2, mem(arg(8))));
				size += 4;
			}
		}
	}
	TNode* l = new TNode(IR_GLOBAL, 0, 0, func);
	return new TNode(IR_FCALL, l, t, size);
}

TNode* Node::seq(TNode* l, TNode* r) {
	return new TNode(IR_SEQ, l, r);
}

TNode* Node::move(TNode* src, TNode* dest) {
	return new TNode(IR_MOVE, src, dest);
}

TNode* Node::global(const std::string& s) {
	return new TNode(IR_GLOBAL, 0, 0, s);
}

TNode* Node::local(int offset) {
	return new TNode(IR_LOCAL, 0, 0, offset);
}

TNode* Node::arg(int offset) {
	return new TNode(IR_ARG, 0, 0, offset);
}

TNode* Node::mem(TNode* ref) {
	return new TNode(IR_MEM, ref, 0);
}

TNode* Node::add(TNode* l, TNode* r) {
	return new TNode(IR_ADD, l, r);
}

TNode* Node::mul(TNode* l, TNode* r) {
	return new TNode(IR_MUL, l, r);
}

TNode* Node::iconst(int n) {
	return new TNode(IR_CONST, 0, 0, n);
}

TNode* Node::ret() {
	return new TNode(IR_RET, 0, 0);
}

TNode* Node::jsr(const std::string& s) {
	return new TNode(IR_JSR, 0, 0, s);
}

TNode* Node::jump(const std::string& s) {
	return new TNode(IR_JUMP, 0, 0, s);
}

TNode* Node::jumpt(TNode* expr, const std::string& s) {
	return new TNode(IR_JUMPT, expr, 0, s);
}

TNode* Node::jumpf(TNode* expr, const std::string& s) {
	return new TNode(IR_JUMPF, expr, 0, s);
}

TNode* Node::jumpge(TNode* l, TNode* r, const std::string& s) {
	return new TNode(IR_JUMPGE, l, r, s);
}