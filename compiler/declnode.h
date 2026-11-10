#ifndef DECLNODE_H
#define DECLNODE_H

struct DeclNode : public Node {
	DECLARE_ARENA_ALLOCATION()

	int pos;
	std::string file;
	DeclNode() :pos(-1) {}
	virtual void proto(DeclSeq* d, Environ* e) {}
	virtual void semant(Environ* e) {}
	virtual void translate(Codegen* g) {}
	virtual void transdata(Codegen* g) {}
};

struct DeclSeqNode : public Node {
	DECLARE_ARENA_ALLOCATION()

	std::vector<DeclNode*> decls;
	DeclSeqNode() {}
	~DeclSeqNode() { for(; decls.size(); decls.pop_back())delete decls.back(); }
	void proto(DeclSeq* d, Environ* e);
	void semant(Environ* e);
	void translate(Codegen* g);
	void transdata(Codegen* g);
	void push_back(DeclNode* d) { decls.push_back(d); }
	int  size() { return decls.size(); }
};

#include "exprnode.h"
#include "stmtnode.h"

//'kind' shouldn't really be in Parser...
//should probably be LocalDeclNode,GlobalDeclNode,ParamDeclNode
struct VarDeclNode : public DeclNode {
	std::string ident, tag;
	int kind; bool constant;
	ExprNode* expr;
	DeclVarNode* sem_var;
	std::vector<std::string>* funcPtrParamTags;
	VarDeclNode(const std::string& i, const std::string& t, int k, bool c, ExprNode* e)
		:ident(i), tag(t), kind(k), constant(c), expr(e), sem_var(0)
		, funcPtrParamTags(0)
	{}
	~VarDeclNode() {
		delete expr; delete sem_var;
		delete funcPtrParamTags;
	}
	void proto(DeclSeq* d, Environ* e);
	void semant(Environ* e);
	void translate(Codegen* g);
};

struct FuncDeclNode : public DeclNode {
	std::string ident, tag;
	DeclSeqNode* params;
	StmtSeqNode* stmts;
	FuncType* sem_type;
	Environ* sem_env;
	FuncDeclNode(const std::string& i, const std::string& t, DeclSeqNode* p, StmtSeqNode* ss) :ident(i), tag(t), params(p), stmts(ss) {}
	~FuncDeclNode() { delete params; delete stmts; }
	void proto(DeclSeq* d, Environ* e);
	void semant(Environ* e);
	void translate(Codegen* g);
};

struct StructDeclNode : public DeclNode {
	std::string ident;
	DeclSeqNode* fields;
	StructType* sem_type;
	StructDeclNode(const std::string& i, DeclSeqNode* f) :ident(i), fields(f) {}
	~StructDeclNode() { delete fields; }
	void proto(DeclSeq* d, Environ* e);
	void semant(Environ* e);
	void translate(Codegen* g);
};

struct DataDeclNode : public DeclNode {
	ExprNode* expr;
	std::string str_label;
	DataDeclNode(ExprNode* e) :expr(e) {}
	~DataDeclNode() { delete expr; }
	void proto(DeclSeq* d, Environ* e);
	void semant(Environ* e);
	void translate(Codegen* g);
	void transdata(Codegen* g);
};

struct VectorDeclNode : public DeclNode {
	std::string ident, tag;
	ExprSeqNode* exprs;
	int kind;
	VectorType* sem_type;
	VectorDeclNode(const std::string& i, const std::string& t, ExprSeqNode* e, int k) :ident(i), tag(t), exprs(e), kind(k) {}
	~VectorDeclNode() { delete exprs; }
	void proto(DeclSeq* d, Environ* e);
	void translate(Codegen* g);
};

struct EnumDeclNode : public DeclNode {
	std::string name;
	std::vector<std::pair<std::string, ExprNode*>> members;

	EnumDeclNode(const std::string& n) : name(n) {}
	~EnumDeclNode() { for (auto& m : members) delete m.second; }

	void proto(DeclSeq* d, Environ* e) override;
	void semant(Environ* e) override {}
	void translate(Codegen* g) override {}
	void transdata(Codegen* g) override {}
};
#endif