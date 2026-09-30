/*
  The parser builds an abstact syntax tree from input tokens.
*/

#ifndef PARSER_H
#define PARSER_H

#include "toker.h"
#include "nodes.h"
#include <vector>

class Parser {
public:

	Parser(Toker& t);

	ProgNode* parse(const std::string& main, bool debug);
	ProgNode* parseAll(const std::string& main, bool debug, std::vector<Ex>& out);

private:
	std::string incfile;
	std::set<std::string> included;
	int depth = 0;
	Toker* toker, * main_toker;
	std::map<std::string, DimNode*> arrayDecls;
	std::vector<DeclVarNode*> withStack;
	std::map<std::string, bool> funcPtrIdents;

	DeclSeqNode* consts;
	DeclSeqNode* structs;
	DeclSeqNode* funcs;
	DeclSeqNode* datas;
	DeclSeqNode* enums;

	void syncToStmtEnd();

	StmtSeqNode* parseStmtSeq(int scope, bool debug);
	void parseStmtSeq(StmtSeqNode* stmts, int scope, bool debug);
	StmtNode* parseAssignment(VarNode* var);
	bool isSoftKeyword(int c);
	StmtNode* parseIdentStatement(const std::string& ident, bool debug);
	ExprNode* parsePrimaryIdent(const std::string& ident);

	void ex(const std::string& s);
	void exp(const std::string& s);

	std::string parseIdent();
	void parseChar(int c);
	std::string parseTypeTag();

	VarNode* parseVar();
	VarNode* parseVar(const std::string& ident, const std::string& tag);
	IfNode* parseIf(bool debug);

	DeclNode* parseVarDecl(int kind, bool constant);
	DimNode* parseArrayDecl();
	DeclNode* parseFuncDecl(bool debug);
	DeclNode* parseStructDecl();
	DeclNode* parseEnumDecl();

	std::vector<std::string>* parseFuncPtrParamTags();

	ExprSeqNode* parseExprSeq();
	bool isParenList();
	ExprSeqNode* parseParenExprList();
	ExprNode* parseTernaryExpr();
	ExprNode* parseTernaryTail();
	ExprNode* parseSelectExpr();

	ExprNode* parseExpr(bool opt);
	ExprNode* parseExpr1(bool opt);		//Or, Xor
	ExprNode* parseExpr1AND(bool opt);	//And
	ExprNode* parseExpr2(bool opt);	//<,=,>,<=,<>,>=
	ExprNode* parseExpr3(bool opt);	//+,-
	ExprNode* parseExpr4(bool opt);	//Lsr,Lsr,Asr
	ExprNode* parseExpr5(bool opt);	//*,/,Mod
	ExprNode* parseExpr6(bool opt);	//^
	ExprNode* parseUniExpr(bool opt);	//+,-,Not,~
	ExprNode* parsePrimary(bool opt);
};

#endif