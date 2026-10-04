#include "../std.h"
#include "codegen_llvm.h"

#include <algorithm>

static bool isFloatOp(int op) {
	switch (op) {
	case IR_FCALL:
	case IR_FRETURN:
	case IR_FCAST:
	case IR_FNEG:
	case IR_FADD:
	case IR_FSUB:
	case IR_FMUL:
	case IR_FDIV:
	case IR_FABS:
	case IR_FSGN:
	case IR_FPOWTWO:
	case IR_FSETEQ:
	case IR_FSETNE:
	case IR_FSETLT:
	case IR_FSETGT:
	case IR_FSETLE:
	case IR_FSETGE:
		return true;
	default:
		return false;
	}
}

static std::string llvmEscape(const std::string& s) {
	static const char* hex = "0123456789ABCDEF";
	std::string r;
	for (unsigned char c : s) {
		if (c == '"' || c == '\\') { r += '\\'; r += (char)c; }
		else if (c >= 32 && c < 127) r += (char)c;
		else { r += '\\'; r += hex[c >> 4]; r += hex[c & 15]; }
	}
	return r;
}

Codegen_llvm::Codegen_llvm(std::ostream& out, bool debug) :Codegen(out, debug) {
	tmpCount = blockCount = dataCount = 0;
	inCode = false;
	blockOpen = false;
	retFloat = false;
	skipNextJump = false;
	frameAlloca = retvalAlloca = false;
	finalized = false;
}

Codegen_llvm::~Codegen_llvm() {
	finalize();
}

std::string Codegen_llvm::newTmp() {
	return "%t" + std::to_string(tmpCount++);
}

std::string Codegen_llvm::newBlock() {
	return "bb" + std::to_string(blockCount++);
}

std::string Codegen_llvm::quoteName(const std::string& s) {
	return "@\"" + llvmEscape(s) + "\"";
}

void Codegen_llvm::emitInstr(const std::string& s) {
	if (!blockOpen) startBlock(newBlock());
	funcBody += "  " + s + "\n";
}

void Codegen_llvm::emitTerm(const std::string& s) {
	if (!blockOpen) startBlock(newBlock());
	funcBody += "  " + s + "\n";
	blockOpen = false;
}

void Codegen_llvm::startBlock(const std::string& l) {
	if (blockOpen) funcBody += "  br label %" + l + "\n";
	funcBody += l + ":\n";
	curBlock = l;
	blockOpen = true;
}

void Codegen_llvm::enter(const std::string& l, int frameSize) {
	funcName = l;
	funcBody.clear();
	funcPre.clear();
	tmpCount = blockCount = 0;
	usedLocals.clear();
	usedParams.clear();
	argSlots.clear();
	retFloat = false;
	skipNextJump = false;
	mainReturnLabel.clear();
	inCode = true;
	curBlock = "entry";
	blockOpen = true;
	frameAlloca = false;
	retvalAlloca = true;
	funcPre = "  %retval = alloca i32, align 4\n";
}

void Codegen_llvm::code(TNode* stmt) {
	if (!stmt) return;
	munch(stmt);
	delete stmt;
}

std::string Codegen_llvm::ensureLocal(int idx) {
	std::string n = "loc" + std::to_string(idx);
	if (!usedLocals.count(idx)) {
		usedLocals.insert(idx);
		funcPre += "  %" + n + " = alloca i32, align 4\n";
	}
	return "%" + n;
}

std::string Codegen_llvm::ensureParam(int idx) {
	std::string n = "arg" + std::to_string(idx);
	if (!usedParams.count(idx)) {
		usedParams.insert(idx);
		funcPre += "  %" + n + " = alloca i32, align 4\n";
		funcPre += "  store i32 %a" + std::to_string(idx) + ", ptr %" + n + "\n";
	}
	return "%" + n;
}

std::string Codegen_llvm::ensureFrame() {
	if (!frameAlloca) {
		frameAlloca = true;
		funcPre += "  %frame = alloca i32, align 4\n";
	}
	return "%frame";
}

std::string Codegen_llvm::ensureRetval() {
	if (!retvalAlloca) {
		retvalAlloca = true;
		funcPre += "  %retval = alloca i32, align 4\n";
	}
	return "%retval";
}

std::string Codegen_llvm::addrLocal(int offset) {
	std::string p;
	if (offset == 0) p = ensureFrame();
	else if (offset > 0) p = ensureParam((offset - 20) / 4);
	else p = ensureLocal((-offset - 4) / 4);
	std::string t = newTmp();
	emitInstr(t + " = ptrtoint ptr " + p + " to i32");
	return t;
}

std::string Codegen_llvm::addrArg(int offset) {
	int idx = offset / 4;
	std::string n = "args" + std::to_string(idx);
	if (!argSlots.count(idx)) {
		argSlots.insert(idx);
		funcPre += "  %" + n + " = alloca i32, align 4\n";
	}
	std::string t = newTmp();
	emitInstr(t + " = ptrtoint ptr %" + n + " to i32");
	return t;
}

std::string Codegen_llvm::addrGlobal(const std::string& name) {
	refGlobals.insert(name);
	std::string t = newTmp();
	emitInstr(t + " = ptrtoint ptr " + quoteName(name) + " to i32");
	return t;
}

Codegen_llvm::Val Codegen_llvm::munch(TNode* t) {
	if (!t) return Val();
	return eval(t, isFloatOp(t->op) ? K_FLOAT : K_INT);
}

Codegen_llvm::Val Codegen_llvm::eval(TNode* t, Kind want) {
	if (!t) return Val("0", want);
	Kind nat = isFloatOp(t->op) ? K_FLOAT : K_INT;
	Val v = gen(t, nat);
	if (v.kind == want) return v;
	std::string r = newTmp();
	if (want == K_FLOAT) emitInstr(r + " = bitcast i32 " + v.v + " to float");
	else emitInstr(r + " = bitcast float " + v.v + " to i32");
	return Val(r, want);
}

Codegen_llvm::Val Codegen_llvm::binInt(const std::string& op, TNode* l, TNode* r) {
	std::string a = eval(l, K_INT).v;
	std::string b = eval(r, K_INT).v;
	std::string t = newTmp();
	emitInstr(t + " = " + op + " i32 " + a + ", " + b);
	return Val(t, K_INT);
}

Codegen_llvm::Val Codegen_llvm::cmpInt(const std::string& pred, TNode* l, TNode* r) {
	std::string a = eval(l, K_INT).v;
	std::string b = eval(r, K_INT).v;
	std::string c = newTmp();
	emitInstr(c + " = icmp " + pred + " i32 " + a + ", " + b);
	std::string t = newTmp();
	emitInstr(t + " = zext i1 " + c + " to i32");
	return Val(t, K_INT);
}

Codegen_llvm::Val Codegen_llvm::cmpFloat(const std::string& pred, TNode* l, TNode* r) {
	std::string a = eval(l, K_FLOAT).v;
	std::string b = eval(r, K_FLOAT).v;
	std::string c = newTmp();
	emitInstr(c + " = fcmp " + pred + " float " + a + ", " + b);
	std::string t = newTmp();
	emitInstr(t + " = zext i1 " + c + " to i32");
	return Val(t, K_INT);
}

Codegen_llvm::Val Codegen_llvm::bitUnary(const std::string& op, TNode* a) {
	std::string x = eval(a, K_INT).v;
	std::string t = newTmp();
	emitInstr(t + " = " + op + " i32 " + x);
	return Val(t, K_INT);
}

Codegen_llvm::Val Codegen_llvm::floatUnary(const std::string& op, TNode* a) {
	std::string x = eval(a, K_FLOAT).v;
	std::string t = newTmp();
	emitInstr(t + " = " + op + " float " + x);
	return Val(t, K_FLOAT);
}

Codegen_llvm::Val Codegen_llvm::floatArith(const std::string& op, TNode* l, TNode* r) {
	std::string a = eval(l, K_FLOAT).v;
	std::string b = eval(r, K_FLOAT).v;
	std::string t = newTmp();
	emitInstr(t + " = " + op + " float " + a + ", " + b);
	return Val(t, K_FLOAT);
}

std::vector<TNode*> Codegen_llvm::collectArgs(TNode* t, int& arity) {
	std::vector<std::pair<int, TNode*> > tmp;
	TNode* c = t->r;
	while (c) {
		if (c->op == IR_SEQ) {
			TNode* s = c->l;
			if (s && s->op == IR_MOVE && s->r && s->r->op == IR_MEM && s->r->l && s->r->l->op == IR_ARG)
				tmp.push_back(std::make_pair(s->r->l->iconst, s->l));
			c = c->r;
		}
		else if (c->op == IR_MOVE && c->r && c->r->op == IR_MEM && c->r->l && c->r->l->op == IR_ARG) {
			tmp.push_back(std::make_pair(c->r->l->iconst, c->l));
			break;
		}
		else break;
	}
	std::sort(tmp.begin(), tmp.end());
	arity = t->iconst / 4;
	if ((int)tmp.size() > arity) arity = (int)tmp.size();
	std::vector<TNode*> r;
	for (size_t k = 0; k < tmp.size(); ++k) r.push_back(tmp[k].second);
	return r;
}

Codegen_llvm::Val Codegen_llvm::genCall(TNode* t, Kind want) {
	bool f = t->op == IR_FCALL;
	int arity = 0;
	std::vector<TNode*> args = collectArgs(t, arity);

	std::vector<std::string> av;
	for (size_t k = 0; k < args.size(); ++k) av.push_back(eval(args[k], K_INT).v);

	std::string ret = f ? "float" : "i32";
	std::string argstr;
	for (int k = 0; k < arity; ++k) {
		if (k) argstr += ", ";
		argstr += "i32 " + (k < (int)av.size() ? av[k] : std::string("0"));
	}

	std::string res = newTmp();
	TNode* c = t->l;

	if (c && c->op == IR_GLOBAL) {
		const std::string& name = c->sconst;
		if (!definedFuncs.count(name)) {
			std::map<std::string, std::pair<bool, int> >::iterator it = externFuncs.find(name);
			if (it == externFuncs.end()) externFuncs[name] = std::make_pair(f, arity);
		}
		emitInstr(res + " = call x86_stdcallcc " + ret + " " + quoteName(name) + "(" + argstr + ")");
	}
	else {
		std::string fp = eval(c, K_INT).v;
		std::string p = newTmp();
		emitInstr(p + " = inttoptr i32 " + fp + " to ptr");
		emitInstr(res + " = call x86_stdcallcc " + ret + " " + p + "(" + argstr + ")");
	}

	return Val(res, f ? K_FLOAT : K_INT);
}

Codegen_llvm::Val Codegen_llvm::genMem(TNode* t) {
	std::string a = eval(t->l, K_INT).v;
	std::string p = newTmp();
	emitInstr(p + " = inttoptr i32 " + a + " to ptr");
	std::string v = newTmp();
	emitInstr(v + " = load i32, ptr " + p);
	return Val(v, K_INT);
}

void Codegen_llvm::genStore(TNode* dst, const std::string& val) {
	if (!dst || dst->op != IR_MEM) return;
	std::string a = eval(dst->l, K_INT).v;
	std::string p = newTmp();
	emitInstr(p + " = inttoptr i32 " + a + " to ptr");
	emitInstr("store i32 " + val + ", ptr " + p);
}

Codegen_llvm::Val Codegen_llvm::gen(TNode* t, Kind want) {
	switch (t->op) {
	case IR_CONST:
		return Val(std::to_string(t->iconst), K_INT);

	case IR_GLOBAL:
		return Val(addrGlobal(t->sconst), K_INT);

	case IR_LOCAL:
		return Val(addrLocal(t->iconst), K_INT);

	case IR_ARG:
		return Val(addrArg(t->iconst), K_INT);

	case IR_MEM:
		return genMem(t);

	case IR_MOVE: {
		std::string v = eval(t->l, K_INT).v;
		genStore(t->r, v);
		return Val(v, K_INT);
	}

	case IR_SEQ:
		munch(t->l);
		return eval(t->r, want);

	case IR_CALL:
	case IR_FCALL:
		return genCall(t, want);

	case IR_JSR:
		emitTerm("br label %" + t->sconst);
		skipNextJump = true;
		return Val();

	case IR_JUMP:
		if (skipNextJump) {
			skipNextJump = false;
			mainReturnLabel = t->sconst;
			return Val();
		}
		emitTerm("br label %" + t->sconst);
		return Val();

	case IR_RET:
		if (funcName == "__MAIN" && !mainReturnLabel.empty()) {
			emitTerm("br label %" + mainReturnLabel);
		}
		else {
			std::string rv = newTmp();
			emitInstr(rv + " = load i32, ptr " + ensureRetval());
			if (retFloat) {
				std::string fv = newTmp();
				emitInstr(fv + " = bitcast i32 " + rv + " to float");
				emitTerm("ret float " + fv);
			}
			else emitTerm("ret i32 " + rv);
		}
		return Val();

	case IR_RETURN: {
		std::string v = eval(t->l, K_INT).v;
		emitInstr("store i32 " + v + ", ptr " + ensureRetval());
		emitTerm("br label %" + t->sconst);
		return Val();
	}

	case IR_FRETURN: {
		retFloat = true;
		std::string v = eval(t->l, K_FLOAT).v;
		std::string b = newTmp();
		emitInstr(b + " = bitcast float " + v + " to i32");
		emitInstr("store i32 " + b + ", ptr " + ensureRetval());
		emitTerm("br label %" + t->sconst);
		return Val();
	}

	case IR_JUMPT: {
		std::string c = eval(t->l, K_INT).v;
		std::string b = newTmp();
		emitInstr(b + " = icmp ne i32 " + c + ", 0");
		std::string nb = newBlock();
		emitTerm("br i1 " + b + ", label %" + t->sconst + ", label %" + nb);
		startBlock(nb);
		return Val();
	}

	case IR_JUMPF: {
		std::string c = eval(t->l, K_INT).v;
		std::string b = newTmp();
		emitInstr(b + " = icmp eq i32 " + c + ", 0");
		std::string nb = newBlock();
		emitTerm("br i1 " + b + ", label %" + t->sconst + ", label %" + nb);
		startBlock(nb);
		return Val();
	}

	case IR_JUMPGE: {
		std::string a = eval(t->l, K_INT).v;
		std::string b = eval(t->r, K_INT).v;
		std::string c = newTmp();
		emitInstr(c + " = icmp uge i32 " + a + ", " + b);
		std::string nb = newBlock();
		emitTerm("br i1 " + c + ", label %" + t->sconst + ", label %" + nb);
		startBlock(nb);
		return Val();
	}

	case IR_NEG: {
		std::string x = eval(t->l, K_INT).v;
		std::string r = newTmp();
		emitInstr(r + " = sub i32 0, " + x);
		return Val(r, K_INT);
	}

	case IR_ABS: {
		std::string x = eval(t->l, K_INT).v;
		std::string c = newTmp();
		emitInstr(c + " = icmp slt i32 " + x + ", 0");
		std::string n = newTmp();
		emitInstr(n + " = sub i32 0, " + x);
		std::string r = newTmp();
		emitInstr(r + " = select i1 " + c + ", i32 " + n + ", i32 " + x);
		return Val(r, K_INT);
	}

	case IR_SGN: {
		std::string x = eval(t->l, K_INT).v;
		std::string ltz = newTmp();
		emitInstr(ltz + " = icmp slt i32 " + x + ", 0");
		std::string gtz = newTmp();
		emitInstr(gtz + " = icmp sgt i32 " + x + ", 0");
		std::string s = newTmp();
		emitInstr(s + " = select i1 " + gtz + ", i32 1, i32 0");
		std::string r = newTmp();
		emitInstr(r + " = select i1 " + ltz + ", i32 -1, i32 " + s);
		return Val(r, K_INT);
	}

	case IR_POWTWO: {
		std::string x = eval(t->l, K_INT).v;
		std::string r = newTmp();
		emitInstr(r + " = mul i32 " + x + ", " + x);
		return Val(r, K_INT);
	}

	case IR_ADD: return binInt("add", t->l, t->r);
	case IR_SUB: return binInt("sub", t->l, t->r);
	case IR_MUL: return binInt("mul", t->l, t->r);
	case IR_DIV: return binInt("sdiv", t->l, t->r);
	case IR_MOD: return binInt("srem", t->l, t->r);
	case IR_AND: return binInt("and", t->l, t->r);
	case IR_OR:
	case IR_LOR: return binInt("or", t->l, t->r);
	case IR_XOR: return binInt("xor", t->l, t->r);
	case IR_SHL: return binInt("shl", t->l, t->r);
	case IR_SHR: return binInt("lshr", t->l, t->r);
	case IR_SAR: return binInt("ashr", t->l, t->r);

	case IR_SETEQ: return cmpInt("eq", t->l, t->r);
	case IR_SETNE: return cmpInt("ne", t->l, t->r);
	case IR_SETLT: return cmpInt("slt", t->l, t->r);
	case IR_SETGT: return cmpInt("sgt", t->l, t->r);
	case IR_SETLE: return cmpInt("sle", t->l, t->r);
	case IR_SETGE: return cmpInt("sge", t->l, t->r);

	case IR_CAST: {
		std::string f = eval(t->l, K_FLOAT).v;
		std::string r = newTmp();
		emitInstr(r + " = fptosi float " + f + " to i32");
		return Val(r, K_INT);
	}

	case IR_FCAST: {
		std::string i = eval(t->l, K_INT).v;
		std::string r = newTmp();
		emitInstr(r + " = sitofp i32 " + i + " to float");
		return Val(r, K_FLOAT);
	}

	case IR_FNEG: return floatUnary("fneg", t->l);
	case IR_FADD: return floatArith("fadd", t->l, t->r);
	case IR_FSUB: return floatArith("fsub", t->l, t->r);
	case IR_FMUL: return floatArith("fmul", t->l, t->r);
	case IR_FDIV: return floatArith("fdiv", t->l, t->r);

	case IR_FABS: {
		std::string x = eval(t->l, K_FLOAT).v;
		std::string b = newTmp();
		emitInstr(b + " = bitcast float " + x + " to i32");
		std::string a = newTmp();
		emitInstr(a + " = and i32 " + b + ", 2147483647");
		std::string r = newTmp();
		emitInstr(r + " = bitcast i32 " + a + " to float");
		return Val(r, K_FLOAT);
	}

	case IR_FSGN: {
		std::string x = eval(t->l, K_FLOAT).v;
		std::string z = newTmp();
		emitInstr(z + " = fcmp oeq float " + x + ", 0.000000e+00");
		std::string ln = newTmp();
		emitInstr(ln + " = fcmp olt float " + x + ", 0.000000e+00");
		std::string s = newTmp();
		emitInstr(s + " = select i1 " + ln + ", float -1.000000e+00, float 1.000000e+00");
		std::string r = newTmp();
		emitInstr(r + " = select i1 " + z + ", float 0.000000e+00, float " + s);
		return Val(r, K_FLOAT);
	}

	case IR_FPOWTWO: {
		std::string x = eval(t->l, K_FLOAT).v;
		std::string r = newTmp();
		emitInstr(r + " = fmul float " + x + ", " + x);
		return Val(r, K_FLOAT);
	}

	case IR_FSETEQ: return cmpFloat("oeq", t->l, t->r);
	case IR_FSETNE: return cmpFloat("one", t->l, t->r);
	case IR_FSETLT: return cmpFloat("olt", t->l, t->r);
	case IR_FSETGT: return cmpFloat("ogt", t->l, t->r);
	case IR_FSETLE: return cmpFloat("ole", t->l, t->r);
	case IR_FSETGE: return cmpFloat("oge", t->l, t->r);

	default:
		break;
	}
	return Val();
}

void Codegen_llvm::leave(TNode* cleanup, int pop_sz) {
	if (cleanup) {
		munch(cleanup);
		delete cleanup;
	}

	int paramCount = pop_sz / 4;

	std::string ret = retFloat ? "float" : "i32";

	if (blockOpen) {
		std::string rv = newTmp();
		emitInstr(rv + " = load i32, ptr " + ensureRetval());
		if (retFloat) {
			std::string fv = newTmp();
			emitInstr(fv + " = bitcast i32 " + rv + " to float");
			emitTerm("ret float " + fv);
		}
		else emitTerm("ret i32 " + rv);
	}

	out << "define x86_stdcallcc " << ret << " " << quoteName(funcName) << "(";
	for (int k = 0; k < paramCount; ++k) {
		if (k) out << ", ";
		out << "i32 %a" << k;
	}
	out << ") {\n";
	out << "entry:\n";
	out << funcPre;
	out << "  store i32 0, ptr " << ensureRetval() << "\n";
	out << funcBody;
	out << "}\n\n";

	definedFuncs.insert(funcName);
	inCode = false;
}

void Codegen_llvm::label(const std::string& l) {
	if (inCode) {
		startBlock(l);
	}
	else {
		pendingDataLabel = l;
	}
}

std::string Codegen_llvm::takeLabel(const std::string& explicitLabel) {
	std::string n = explicitLabel;
	if (n.empty()) {
		if (!pendingDataLabel.empty()) n = pendingDataLabel;
		else n = "dat" + std::to_string(dataCount++);
	}
	pendingDataLabel.clear();
	while (definedData.count(n)) n += "_";
	definedData.insert(n);
	return n;
}

void Codegen_llvm::i_data(int i, const std::string& l) {
	std::string n = takeLabel(l);
	out << quoteName(n) << " = global i32 " << i << ", align 4\n";
}

void Codegen_llvm::s_data(const std::string& s, const std::string& l) {
	std::string n = takeLabel(l);
	out << quoteName(n) << " = private unnamed_addr constant [" << (s.size() + 1) << " x i8] c\""
		<< llvmEscape(s) << "\\00\", align 1\n";
}

void Codegen_llvm::p_data(const std::string& p, const std::string& l) {
	std::string n = takeLabel(l);
	refGlobals.insert(p);
	out << quoteName(n) << " = global i32 ptrtoint (ptr " << quoteName(p) << " to i32), align 4\n";
}

void Codegen_llvm::align_data(int n) {
}

void Codegen_llvm::flush() {
}

void Codegen_llvm::finalize() {
	if (finalized) return;
	finalized = true;

	for (std::map<std::string, std::pair<bool, int> >::iterator it = externFuncs.begin(); it != externFuncs.end(); ++it) {
		const std::string& name = it->first;
		if (definedFuncs.count(name) || emittedFuncs.count(name)) continue;
		emittedFuncs.insert(name);
		out << "declare x86_stdcallcc " << (it->second.first ? "float" : "i32") << " " << quoteName(name) << "(";
		for (int k = 0; k < it->second.second; ++k) {
			if (k) out << ", ";
			out << "i32";
		}
		out << ")\n";
	}

	for (std::set<std::string>::iterator it = refGlobals.begin(); it != refGlobals.end(); ++it) {
		const std::string& name = *it;
		if (definedData.count(name) || definedFuncs.count(name) || emittedGlobals.count(name)) continue;
		emittedGlobals.insert(name);
		out << quoteName(name) << " = external global i32, align 4\n";
	}
}
