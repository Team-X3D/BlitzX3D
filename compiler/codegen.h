#ifndef CODEGEN_H
#define CODEGEN_H

#include "std.h"

enum {
	IR_JUMP, IR_JUMPT, IR_JUMPF, IR_JUMPGE,

	IR_SEQ, IR_MOVE, IR_MEM, IR_LOCAL, IR_GLOBAL, IR_ARG, IR_CONST,

	IR_JSR, IR_RET, IR_AND, IR_OR, IR_LOR, IR_XOR, IR_SHL, IR_SHR, IR_SAR,

	IR_CALL, IR_RETURN, IR_CAST,
	IR_NEG, IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD, IR_ABS, IR_SGN, IR_POWTWO,
	IR_SETEQ, IR_SETNE, IR_SETLT, IR_SETGT, IR_SETLE, IR_SETGE,

	IR_FCALL, IR_FRETURN, IR_FCAST,
	IR_FNEG, IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV, IR_FABS, IR_FSGN, IR_FPOWTWO,
	IR_FSETEQ, IR_FSETNE, IR_FSETLT, IR_FSETGT, IR_FSETLE, IR_FSETGE,
};

struct TNode {

	int op;				//opcode
	TNode* l, * r;		//args
	int iconst;			//for CONST type_int
	std::string sconst;		//for CONST type_string

	TNode(int op, TNode* l = 0, TNode* r = 0) :op(op), l(l), r(r), iconst(0) {}
	TNode(int op, TNode* l, TNode* r, int i) :op(op), l(l), r(r), iconst(i) {}
	TNode(int op, TNode* l, TNode* r, const std::string& s) :op(op), l(l), r(r), iconst(0), sconst(s) {}
	~TNode() { delete l; delete r; }
};

class Codegen {
public:
	std::ostream& out;
	bool debug;
	Codegen(std::ostream& out, bool debug) :out(out), debug(debug) {}
	virtual ~Codegen() {}

	virtual void enter(const std::string& l, int frameSize) = 0;
	virtual void code(TNode* code) = 0;
	virtual void leave(TNode* cleanup, int pop_sz) = 0;
	virtual void label(const std::string& l) = 0;
	virtual void i_data(int i, const std::string& l = "") = 0;
	virtual void s_data(const std::string& s, const std::string& l = "") = 0;
	virtual void p_data(const std::string& p, const std::string& l = "") = 0;
	virtual void align_data(int n) = 0;
	virtual void flush() = 0;
};

#endif