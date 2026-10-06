#ifndef EXPRNODE_H
#define EXPRNODE_H

#include "node.h"

struct ConstNode;	//is constant int,float or string

struct ExprNode : public Node {
	DECLARE_ARENA_ALLOCATION()

	Type* sem_type;
	ExprNode() :sem_type(0) {}
	ExprNode(Type* t) :sem_type(t) {}

	ExprNode* castTo(Type* ty, Environ* e);

	virtual ExprNode* semant(Environ* e) = 0;
	virtual TNode* translate(Codegen* g) = 0;
	virtual ConstNode* constNode() { return 0; }
};

struct ExprSeqNode : public Node {
	DECLARE_ARENA_ALLOCATION()

	std::vector<ExprNode*> exprs;
	~ExprSeqNode() { for(; exprs.size(); exprs.pop_back()) delete exprs.back(); }
	void push_back(ExprNode* e) { exprs.push_back(e); }
	int  size() { return exprs.size(); }
	void semant(Environ* e);
	TNode* translate(Codegen* g, bool userlib);
	void castTo(DeclSeq* ds, Environ* e, bool userlib);
	void castTo(Type* t, Environ* e);
};

#include "varnode.h"

struct CastNode : public ExprNode {
	ExprNode* expr;
	Type* type;
	CastNode(ExprNode* ex, Type* ty) :expr(ex), type(ty) {}
	~CastNode() { delete expr; }
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

struct CallNode : public ExprNode {
	std::string ident, tag;
	ExprSeqNode* exprs;
	Decl* sem_decl;
	CallNode(const std::string& i, const std::string& t, ExprSeqNode* e) :ident(i), tag(t), exprs(e) {}
	~CallNode() { delete exprs; }
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

struct CallPtrNode : public ExprNode {
	std::string ident;
	Decl* sem_decl;
	bool sem_isType;
	CallPtrNode(const std::string& i) :ident(i), sem_decl(nullptr), sem_isType(false) {}
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

struct CallIndirectNode : public ExprNode {
	ExprNode* funcExpr;
	ExprSeqNode* exprs;
	FuncPtrType* sem_fptype;
	CallIndirectNode(ExprNode* f, ExprSeqNode* e) :funcExpr(f), exprs(e), sem_fptype(nullptr) {}
	~CallIndirectNode() { delete funcExpr; delete exprs; }
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

struct VarExprNode : public ExprNode {
	VarNode* var;
	VarExprNode(VarNode* v) :var(v) {}
	~VarExprNode() { delete var; }
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

struct ConstNode : public ExprNode {
	ExprNode* semant(Environ* e) { return this; }
	ConstNode* constNode() { return this; }
	virtual int intValue() = 0;
	virtual float floatValue() = 0;
	virtual std::string stringValue() = 0;
};

struct IntConstNode : public ConstNode {
	int value;
	IntConstNode(int n);
	TNode* translate(Codegen* g);
	int intValue();
	float floatValue();
	std::string stringValue();
};

struct FloatConstNode : public ConstNode {
	float value;
	FloatConstNode(float f);
	TNode* translate(Codegen* g);
	int intValue();
	float floatValue();
	std::string stringValue();
};

struct StringConstNode : public ConstNode {
	std::string value;
	StringConstNode(const std::string& s);
	TNode* translate(Codegen* g);
	int intValue();
	float floatValue();
	std::string stringValue();
};

struct NullConstNode : public ConstNode {
	NullConstNode();
	TNode* translate(Codegen* g);
	int intValue();
	float floatValue();
	std::string stringValue();
};

struct UniExprNode : public ExprNode {
	int op; ExprNode* expr;
	UniExprNode(int op, ExprNode* expr) :op(op), expr(expr) {}
	~UniExprNode() { delete expr; }
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

// and, or, eor, lsl, lsr, asr
struct BinExprNode : public ExprNode {
	int op; ExprNode* lhs, * rhs;
	BinExprNode(int op, ExprNode* lhs, ExprNode* rhs) :op(op), lhs(lhs), rhs(rhs) {}
	~BinExprNode() { delete lhs; delete rhs; }
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

// *,/,Mod,+,-
struct ArithExprNode : public ExprNode {
	int op; ExprNode* lhs, * rhs;
	ArithExprNode(int op, ExprNode* lhs, ExprNode* rhs) :op(op), lhs(lhs), rhs(rhs) {}
	~ArithExprNode() { delete lhs; delete rhs; }
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

//<,=,>,<=,<>,>=
struct RelExprNode : public ExprNode {
	int op; ExprNode* lhs, * rhs;
	Type* opType;
	RelExprNode(int op, ExprNode* lhs, ExprNode* rhs) :op(op), lhs(lhs), rhs(rhs) {}
	~RelExprNode() { delete lhs; delete rhs; }
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

struct NewNode : public ExprNode {
	std::string ident;
	NewNode(const std::string& i) :ident(i) {}
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

struct FirstNode : public ExprNode {
	std::string ident;
	FirstNode(const std::string& i) :ident(i) {}
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

struct LastNode : public ExprNode {
	std::string ident;
	LastNode(const std::string& i) :ident(i) {}
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

struct AfterNode : public ExprNode {
	ExprNode* expr;
	AfterNode(ExprNode* e) :expr(e) {}
	~AfterNode() { delete expr; }
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

struct BeforeNode : public ExprNode {
	ExprNode* expr;
	BeforeNode(ExprNode* e) :expr(e) {}
	~BeforeNode() { delete expr; }
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

struct ObjectCastNode : public ExprNode {
	ExprNode* expr;
	std::string type_ident;
	ObjectCastNode(ExprNode* e, const std::string& t) :expr(e), type_ident(t) {}
	~ObjectCastNode() { delete expr; }
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

struct ObjectHandleNode : public ExprNode {
	ExprNode* expr;
	ObjectHandleNode(ExprNode* e) :expr(e) {}
	~ObjectHandleNode() { delete expr; }
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

struct AddrOfNode : public ExprNode {
	VarNode* var;
	AddrOfNode(VarNode* v) :var(v) {}
	~AddrOfNode() { delete var; }
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

struct OffsetOfNode : public ExprNode {
	std::string typeIdent, fieldIdent;
	int sem_offset;
	OffsetOfNode(const std::string& t, const std::string& f) :typeIdent(t), fieldIdent(f), sem_offset(0) {}
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

struct TypeFieldNode : public ExprNode {
	std::string typeIdent, fieldIdent;
	int sem_index;
	TypeFieldNode(const std::string& t, const std::string& f) :typeIdent(t), fieldIdent(f), sem_index(0) {}
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

struct TernaryExprNode : public ExprNode {
	ExprNode* cond, * thenExpr, * elseExpr;
	VarNode* sem_temp;
	TernaryExprNode(ExprNode* c, ExprNode* t, ExprNode* e) :cond(c), thenExpr(t), elseExpr(e), sem_temp(0) {}
	~TernaryExprNode() { delete cond; delete thenExpr; delete elseExpr; delete sem_temp; }
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

struct SelectExprNode : public ExprNode {
	struct CaseExpr {
		ExprSeqNode* exprs;
		ExprNode* result;
		CaseExpr(ExprSeqNode* e, ExprNode* r) :exprs(e), result(r) {}
	};
	ExprNode* expr;
	std::vector<CaseExpr> cases;
	ExprNode* defaultExpr;
	VarNode* sem_temp, * sem_result;
	Type* op_type;
	SelectExprNode(ExprNode* e) :expr(e), defaultExpr(0), sem_temp(0), sem_result(0), op_type(0) {}
	~SelectExprNode() {
		delete expr; delete defaultExpr; delete sem_temp; delete sem_result;
		for(size_t k = 0; k < cases.size(); ++k) { delete cases[k].exprs; delete cases[k].result; }
	}
	void push_back(ExprSeqNode* e, ExprNode* r) { cases.push_back(CaseExpr(e, r)); }
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

struct IsExprNode : public ExprNode {
	ExprNode* lhs;
	ExprSeqNode* exprs;
	bool negate;
	VarNode* sem_temp;
	Type* op_type;
	IsExprNode(ExprNode* l, ExprSeqNode* e, bool n) :lhs(l), exprs(e), negate(n), sem_temp(0), op_type(0) {}
	~IsExprNode() { delete lhs; delete exprs; delete sem_temp; }
	ExprNode* semant(Environ* e);
	TNode* translate(Codegen* g);
};

#endif