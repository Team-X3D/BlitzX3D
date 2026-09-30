#include "std.h"
#include <cstdlib>
#include "parser.h"
#include "environ.h"
#include "../MultiLang/MultiLang.h"
#include <format>
#include "preprocessor.h"

static const int TEXTLIMIT = 1024 * 1024 - 1;

enum {
	STMTS_PROG, STMTS_BLOCK, STMTS_LINE
};

static bool isTerm(int c) { return c == ':' || c == '\n'; }

Parser::Parser(Toker& t) :toker(&t), main_toker(&t) {
}

ProgNode* Parser::parse(const std::string& main, bool debug) {

	incfile = main;

	consts = new DeclSeqNode();
	structs = new DeclSeqNode();
	funcs = new DeclSeqNode();
	datas = new DeclSeqNode();
	enums = new DeclSeqNode();
	StmtSeqNode* stmts = 0;

	try {
		stmts = parseStmtSeq(STMTS_PROG, debug);
		if (toker->curr() != EOF) exp("end-of-file");
	}
	catch (Ex) {
		delete stmts; delete datas; delete funcs; delete structs; delete consts;
		throw;
	}

	return new ProgNode(consts, structs, funcs, datas, enums, stmts);
}

void Parser::syncToStmtEnd() {
	while (toker->curr() != ':' && toker->curr() != '\n' && toker->curr() != EOF) toker->next();
}

struct IncludeGuard {
	Toker*& toker;
	std::string& incfile;
	Toker* saved_toker;
	std::string saved_inc;
	IncludeGuard(Toker*& t, std::string& f) :toker(t), incfile(f), saved_toker(t), saved_inc(f) {}
	~IncludeGuard() { toker = saved_toker; incfile = saved_inc; }
	void engage(Toker* t, const std::string& f) { toker = t; incfile = f; }
};

ProgNode* Parser::parseAll(const std::string& main, bool debug, std::vector<Ex>& out) {
	Ex_collect_scope collecting(out);
	ProgNode* p = 0;
	try { p = parse(main, debug); }
	catch (Ex& x) {
		if (out.empty()) {
			Ex_fill(x, main_toker->pos(), main);
			out.push_back(x);
		}
		p = 0;
	}
	if (!out.empty()) { delete p; return 0; }
	return p;
}

void Parser::ex(const std::string& s) {
	throw Ex(s, toker->pos(), incfile);
}

void Parser::exp(const std::string& s) {
	switch (toker->curr()) {
	case NEXT:ex(MultiLang::next_without_for);
	case CONTINUE:ex(MultiLang::continue_without_for);
	case WEND:ex(MultiLang::wend_without_while);
	case ELSE:case ELSEIF:ex(MultiLang::else_without_if);
	case ENDIF:ex(MultiLang::endif_without_if);
	case ENDFUNCTION:ex(MultiLang::end_function_without_function);
	case UNTIL:ex(MultiLang::until_without_repeat);
	case FOREVER:ex(MultiLang::forever_without_repeat);
	case CASE:ex(MultiLang::case_without_select);
	case ENDSELECT:ex(MultiLang::end_select_without_select);
	}
	ex(std::format(MultiLang::expecting, s));
}

std::string Parser::parseIdent() {
	if (isSoftKeyword(toker->curr())) {
		std::string t = tolower(toker->text());
		toker->next();
		return t;
	}
	if (toker->curr() != IDENT) exp(MultiLang::identifier);
	std::string t = toker->text();
	toker->next();
	return t;
}

bool Parser::isSoftKeyword(int c) {
	return c == DO || c == LOOP || c == WITH || c == IS || c == ISNOT;
}

StmtNode* Parser::parseIdentStatement(const std::string& ident, bool debug) {
	std::string tag = parseTypeTag();
	bool isFuncPtrCall = funcPtrIdents.count(ident) != 0 && toker->curr() == '('
		&& arrayDecls.find(ident) == arrayDecls.end();
	if (arrayDecls.find(ident) == arrayDecls.end()
		&& toker->curr() != '=' && toker->curr() != '\\' && toker->curr() != '['
		&& toker->curr() != PLUSEQ && toker->curr() != MINUSEQ
		&& toker->curr() != STAREQ && toker->curr() != SLASHEQ
		&& toker->curr() != AMPEQ
		&& !isFuncPtrCall
		) {
		ExprSeqNode* exprs;
		if (toker->curr() == '(') {
			int nest = 1, k;
			for (k = 1;; ++k) {
				int c = toker->lookAhead(k);
				if (isTerm(c)) ex(MultiLang::mismatched_brackets);
				else if (c == '(') ++nest;
				else if (c == ')' && !--nest) break;
			}
			if (isTerm(toker->lookAhead(++k))) {
				toker->next();
				exprs = parseExprSeq();
				if (toker->curr() != ')') exp("')'");
				toker->next();
			}
			else exprs = parseExprSeq();
		}
		else exprs = parseExprSeq();
		if ((ident != "debuglog" && ident != "stop") || debug) {
			CallNode* call = new CallNode(ident, tag, exprs);
			return new ExprStmtNode(call);
		}
		return 0;
	}
	else if (isFuncPtrCall) {
		toker->next();
		std::unique_ptr<ExprSeqNode> exprs(parseExprSeq());
		if (toker->curr() != ')') exp("')'");
		toker->next();
		VarNode* var = new IdentVarNode(ident, tag);
		ExprNode* funcExpr = new VarExprNode(var);
		CallIndirectNode* call = new CallIndirectNode(funcExpr, exprs.release());
		return new ExprStmtNode(call);
	}
	else {
		VarNode* var = parseVar(ident, tag);
		return parseAssignment(var);
	}
}

ExprNode* Parser::parsePrimaryIdent(const std::string& ident) {
	std::string tag = parseTypeTag();
	ExprNode* result = 0;
	if (funcPtrIdents.count(ident) && toker->curr() == '(' && arrayDecls.find(ident) == arrayDecls.end()) {
		toker->next();
		std::unique_ptr<ExprSeqNode> exprs(parseExprSeq());
		if (toker->curr() != ')') exp("')'");
		toker->next();
		VarNode* var = new IdentVarNode(ident, tag);
		ExprNode* funcExpr = new VarExprNode(var);
		result = new CallIndirectNode(funcExpr, exprs.release());
	}
	else if (toker->curr() == '(' && arrayDecls.find(ident) == arrayDecls.end()) {
		toker->next();
		std::unique_ptr<ExprSeqNode> exprs(parseExprSeq());
		if (toker->curr() != ')') exp("')'");
		toker->next();
		result = new CallNode(ident, tag, exprs.release());
	}
	else {
		VarNode* var = parseVar(ident, tag);
		result = new VarExprNode(var);
	}
	return result;
}

void Parser::parseChar(int c) {
	if (toker->curr() != c) exp(std::string("'") + char(c) + std::string("'"));
	toker->next();
}

StmtSeqNode* Parser::parseStmtSeq(int scope, bool debug) {
	std::unique_ptr<StmtSeqNode> stmts(new StmtSeqNode(incfile));
	parseStmtSeq(stmts.get(), scope, debug);
	return stmts.release();
}

void Parser::parseStmtSeq(StmtSeqNode* stmts, int scope, bool debug) {

	for (;;) {
		while (toker->curr() == ':' || (scope != STMTS_LINE && toker->curr() == '\n')) toker->next();
		StmtNode* result = 0;

		int pos = toker->pos();

		try {
		int curToken = toker->curr();
		if (!experimentalSyntaxEnabled && isSoftKeyword(curToken)) curToken = IDENT;
		switch (curToken) {
		case INCLUDE:
		{
			if (toker->next() != STRINGCONST) exp(MultiLang::include_filename);
			std::string inc = toker->text(); toker->next();
			inc = inc.substr(1, inc.size() - 2);

			//WIN32 KLUDGE//
			char buff[MAX_PATH], * p;
			if (GetFullPathName(inc.c_str(), MAX_PATH, buff, &p)) inc = buff;
			inc = tolower(inc);

			if (included.find(inc) != included.end()) break;

			std::ifstream i_stream(inc);
			if (!i_stream.good()) ex(MultiLang::unable_open_include_file);

			Toker i_toker(inc, i_stream, debug);

			IncludeGuard include_guard(toker, incfile);
			include_guard.engage(&i_toker, inc);

			included.insert(incfile);

			std::unique_ptr<StmtSeqNode> ss(parseStmtSeq(scope, debug));
			if (toker->curr() != EOF) exp(MultiLang::end_of_file);

			result = new IncludeNode(incfile, ss.release());
		}
		break;
		case IDENT:
		{
			std::string ident = toker->text();
			toker->next();
			if (experimentalSyntaxEnabled) {
				result = parseIdentStatement(ident, debug);
			}
			else {
			std::string tag = parseTypeTag();
			if (arrayDecls.find(ident) == arrayDecls.end()
				&& toker->curr() != '=' && toker->curr() != '\\' && toker->curr() != '['
				) {
				//must be a function
				ExprSeqNode* exprs;
				if (toker->curr() == '(') {
					//ugly lookahead for optional '()' around statement params
					int nest = 1, k;
					for (k = 1;; ++k) {
						int c = toker->lookAhead(k);
						if (isTerm(c)) ex(MultiLang::mismatched_brackets);
						else if (c == '(') ++nest;
						else if (c == ')' && !--nest) break;
					}
					if (isTerm(toker->lookAhead(++k))) {
						toker->next();
						exprs = parseExprSeq();
						if (toker->curr() != ')') exp("')'");
						toker->next();
					}
					else exprs = parseExprSeq();
				}
				else exprs = parseExprSeq();
				if ((ident != "debuglog" && ident != "stop") || debug) {
					CallNode* call = new CallNode(ident, tag, exprs);
					result = new ExprStmtNode(call);
				}
				else { result = 0; }
			}
			else {
				//must be a var
				std::unique_ptr<VarNode> var(parseVar(ident, tag));
				if (toker->curr() != '=') exp(MultiLang::variable_assignment);
				toker->next(); ExprNode* expr = parseExpr(false);
				result = new AssNode(var.release(), expr);
			}
			}
		}
		break;
		case IS:
		case ISNOT:
		{
			std::string ident = tolower(toker->text());
			toker->next();
			result = parseIdentStatement(ident, debug);
		}
		break;
		case LOOP:
		{
			int nxt = toker->lookAhead(1);
			if (nxt == WHILE || nxt == UNTIL || nxt == ':' || nxt == '\n' || nxt == EOF) {
				return;
			}
			std::string ident = tolower(toker->text());
			toker->next();
			result = parseIdentStatement(ident, debug);
			break;
		}
		case IF:
		{
			toker->next(); result = parseIf(debug);
			if (toker->curr() == ENDIF) toker->next();
		}
		break;
		case WHILE:
		{
			toker->next();
			std::unique_ptr<ExprNode> expr(parseExpr(false));
			std::unique_ptr<StmtSeqNode> stmts(parseStmtSeq(STMTS_BLOCK, debug));
			int pos = toker->pos();
			if (toker->curr() != WEND) exp("'Wend'");
			toker->next();
			result = new WhileNode(expr.release(), stmts.release(), pos);
		}
		break;
		case REPEAT:
		{
			toker->next(); ExprNode* expr = 0;
			std::unique_ptr<StmtSeqNode> stmts(parseStmtSeq(STMTS_BLOCK, debug));
			int curr = toker->curr();
			int pos = toker->pos();
			if (curr != UNTIL && curr != FOREVER) exp(MultiLang::until_or_forever);
			toker->next(); if (curr == UNTIL) expr = parseExpr(false);
			result = new RepeatNode(stmts.release(), expr, pos);
		}
		break;
		case SELECT:
		{
			toker->next(); ExprNode* expr = parseExpr(false);
			std::unique_ptr<SelectNode> selNode(new SelectNode(expr));
			for (;;) {
				while (isTerm(toker->curr())) toker->next();
				if (toker->curr() == CASE) {
					toker->next();
					std::unique_ptr<ExprSeqNode> exprs(parseExprSeq());
					if (!exprs->size()) exp(MultiLang::expression_sequence);
					std::unique_ptr<StmtSeqNode> stmts(parseStmtSeq(STMTS_BLOCK, debug));
					selNode->push_back(new CaseNode(exprs.release(), stmts.release()));
					continue;
				}
				else if (toker->curr() == DEFAULT) {
					toker->next();
					std::unique_ptr<StmtSeqNode> stmts(parseStmtSeq(STMTS_BLOCK, debug));
					if (toker->curr() != ENDSELECT) exp("'End Select'");
					selNode->defStmts = stmts.release();
					break;
				}
				else if (toker->curr() == ENDSELECT) {
					break;
				}
				exp(MultiLang::case_default_or_end_select);
			}
			toker->next();
			result = selNode.release();
		}
		break;
		case FOR:
		{
			std::unique_ptr<VarNode> var;
			std::unique_ptr<StmtSeqNode> stmts;
			toker->next();
			var = std::unique_ptr<VarNode>(parseVar());
			if (toker->curr() != '=') exp(MultiLang::variable_assignment);
			if (toker->next() == EACH) {
				toker->next();
				std::string ident = parseIdent();
				stmts = std::unique_ptr<StmtSeqNode>(parseStmtSeq(STMTS_BLOCK, debug));
				int pos = toker->pos();
				if (toker->curr() != NEXT) exp("'Next'");
				toker->next();
				result = new ForEachNode(var.release(), ident, stmts.release(), pos);
			}
			else {
				std::unique_ptr<ExprNode> from, to, step;
				from = std::unique_ptr<ExprNode>(parseExpr(false));
				if (toker->curr() != TO) exp("'TO'");
				toker->next(); to = std::unique_ptr<ExprNode>(parseExpr(false));
				//step...
				if (toker->curr() == STEP) {
					toker->next(); step = std::unique_ptr<ExprNode>(parseExpr(false));
				}
				else step = std::unique_ptr<IntConstNode>(new IntConstNode(1));
				stmts = std::unique_ptr<StmtSeqNode>(parseStmtSeq(STMTS_BLOCK, debug));
				int pos = toker->pos();
				if (toker->curr() != NEXT) exp("'Next'");
				toker->next();
				result = new ForNode(var.release(), from.release(), to.release(), step.release(), stmts.release(), pos);
			}
		}
		break;
		case CONTINUE:
		{
			toker->next(); result = new ContinueNode();
		}
		break;
		case EXIT:
		{
			toker->next(); result = new ExitNode();
		}
		break;
		case GOTO:
		{
			toker->next(); std::string t = parseIdent(); result = new GotoNode(t);
		}
		break;
		case GOSUB:
		{
			toker->next(); std::string t = parseIdent(); result = new GosubNode(t);
		}
		break;
		case RETURN:
		{
			toker->next(); result = new ReturnNode(parseExpr(true));
		}
		break;
		case BBDELETE:
		{
			if (toker->next() == EACH) {
				toker->next(); std::string t = parseIdent();
				result = new DeleteEachNode(t);
			}
			else {
				ExprNode* expr = parseExpr(false);
				result = new DeleteNode(expr);
			}
		}
		break;
		case INSERT:
		{
			toker->next();
			std::unique_ptr<ExprNode> expr1(parseExpr(false));
			if (toker->curr() != BEFORE && toker->curr() != AFTER) exp(MultiLang::before_or_after);
			bool before = toker->curr() == BEFORE; toker->next();
			std::unique_ptr<ExprNode> expr2(parseExpr(false));
			result = new InsertNode(expr1.release(), expr2.release(), before);
		}
		break;
		case READ:
			do {
				toker->next(); VarNode* var = parseVar();
				StmtNode* stmt = new ReadNode(var);
				stmt->pos = pos; pos = toker->pos();
				stmts->push_back(stmt);
			} while (toker->curr() == ',');
			break;
		case RESTORE:
			if (toker->next() == IDENT || (experimentalSyntaxEnabled && isSoftKeyword(toker->curr()))) {
				std::string label = toker->curr() == IDENT ? toker->text() : tolower(toker->text());
				result = new RestoreNode(label); toker->next();
			}
			else result = new RestoreNode("");
			break;
		case DATA:
			if (scope != STMTS_PROG) ex(MultiLang::data_can_only_appear_in_main);
			do {
				toker->next();
				ExprNode* expr = parseExpr(false);
				datas->push_back(new DataDeclNode(expr));
			} while (toker->curr() == ',');
			break;
		case TYPE:
			if (scope != STMTS_PROG) ex(MultiLang::type_can_only_appear_in_main);
			toker->next(); structs->push_back(parseStructDecl());
			break;
		case ENUM:
			if (scope != STMTS_PROG) ex("Enum can only appear in main");
			toker->next();
			enums->push_back(parseEnumDecl());
			break;
		case BBCONST:
			if (scope != STMTS_PROG) ex(MultiLang::const_can_only_appear_in_main);
			do {
				toker->next(); consts->push_back(parseVarDecl(DECL_GLOBAL, true));
			} while (toker->curr() == ',');
			break;
		case FUNCTION:
			if (scope != STMTS_PROG) ex(MultiLang::function_can_only_appear_in_main);
			toker->next(); funcs->push_back(parseFuncDecl(debug));
			break;
		case DIM:
			do {
				toker->next();
				StmtNode* stmt = parseArrayDecl();
				stmt->pos = pos; pos = toker->pos();
				stmts->push_back(stmt);
			} while (toker->curr() == ',');
			break;
		case LOCAL:
			do {
				toker->next();
				DeclNode* d = parseVarDecl(DECL_LOCAL, false);
				StmtNode* stmt = new DeclStmtNode(d);
				stmt->pos = pos; pos = toker->pos();
				stmts->push_back(stmt);
			} while (toker->curr() == ',');
			break;
		case GLOBAL:
			if (scope != STMTS_PROG) ex(MultiLang::global_can_only_appear_in_main);
			do {
				toker->next();
				DeclNode* d = parseVarDecl(DECL_GLOBAL, false);
				StmtNode* stmt = new DeclStmtNode(d);
				stmt->pos = pos; pos = toker->pos();
				stmts->push_back(stmt);
			} while (toker->curr() == ',');
			break;
		case DO:
		{
			int nxt = toker->lookAhead(1);
			if (nxt != WHILE && nxt != UNTIL && nxt != ':' && nxt != '\n' && nxt != EOF) {
				std::string ident = tolower(toker->text());
				toker->next();
				result = parseIdentStatement(ident, debug);
				break;
			}
			if (toker->next() == WHILE || toker->curr() == UNTIL) {
				bool isUntil = (toker->curr() == UNTIL);
				toker->next();
				ExprNode* cond = parseExpr(false);
				StmtSeqNode* body = parseStmtSeq(STMTS_BLOCK, debug);
				if (toker->curr() != LOOP) exp("'Loop'");
				toker->next();
				result = new DoLoopNode(cond, body, true, isUntil);
			}
			else {
				StmtSeqNode* body = parseStmtSeq(STMTS_BLOCK, debug);
				if (toker->curr() != LOOP) exp("'Loop'");
				toker->next();
				bool isUntil = false;
				ExprNode* cond = nullptr;
				if (toker->curr() == WHILE || toker->curr() == UNTIL) {
					isUntil = (toker->curr() == UNTIL);
					toker->next();
					cond = parseExpr(false);
				}
				result = new DoLoopNode(cond, body, false, isUntil);
			}
			break;
		}

		case WITH:
		{
			int nxt = toker->lookAhead(1);
			if (nxt == '=' || nxt == PLUSEQ || nxt == MINUSEQ || nxt == STAREQ
				|| nxt == SLASHEQ || nxt == AMPEQ || nxt == '(' || nxt == '['
				|| nxt == '\\' || nxt == '%' || nxt == '#' || nxt == '$'
				|| nxt == '@' || nxt == '.' || nxt == ':' || nxt == '\n' || nxt == EOF) {
				std::string ident = tolower(toker->text());
				toker->next();
				result = parseIdentStatement(ident, debug);
				break;
			}
			toker->next();
			ExprNode* objExpr = parseExpr(false);
			DeclVarNode* tempVar = new DeclVarNode();
			withStack.push_back(tempVar);
			StmtSeqNode* body = parseStmtSeq(STMTS_BLOCK, debug);
			if (toker->curr() != ENDWITH) exp("'End With'");
			toker->next();
			withStack.pop_back();
			result = new WithNode(objExpr, tempVar, body);
			break;
		}
		case '.':
		{
			toker->next();
			if (experimentalSyntaxEnabled && !withStack.empty()) {
				std::string fieldIdent = parseIdent();
				std::string fieldTag = parseTypeTag();
				if (toker->curr() == '=' || toker->curr() == PLUSEQ || toker->curr() == MINUSEQ || toker->curr() == STAREQ || toker->curr() == SLASHEQ || toker->curr() == AMPEQ) {
					DeclVarNode* tempNode = withStack.back();
					VarExprNode* base = new VarExprNode(tempNode);
					FieldVarNode* fvn = new FieldVarNode(base, fieldIdent, fieldTag);
					result = parseAssignment(fvn);
					break;
				}
				result = new LabelNode(fieldIdent, datas->size());
				break;
			}
			std::string t = parseIdent();
			result = new LabelNode(t, datas->size());
			break;
		}
		break;
		default:
			return;
		}
		}
		catch (Ex& x) {
			if (!Ex_collecting || !Ex_errors) throw;
			Ex_fill(x, toker->pos(), incfile);
			Ex_errors->push_back(x);
			if ((int)Ex_errors->size() >= Ex_MAX_ERRORS) throw;
			syncToStmtEnd();
			continue;
		}

		if (result) {
			result->pos = pos;
			stmts->push_back(result);
		}
	}
}

StmtNode* Parser::parseAssignment(VarNode* var) {
	int opToken = toker->curr();
	if (opToken == '=') {
		toker->next();
		ExprNode* rhs = parseExpr(false);
		return new AssNode(var, rhs);
	}
	else if (experimentalSyntaxEnabled && (opToken == PLUSEQ || opToken == MINUSEQ || opToken == STAREQ || opToken == SLASHEQ || opToken == AMPEQ)) {
		int arithOp = 0;
		switch (opToken) {
		case PLUSEQ: arithOp = '+'; break;
		case MINUSEQ: arithOp = '-'; break;
		case STAREQ: arithOp = '*'; break;
		case SLASHEQ: arithOp = '/'; break;
		case AMPEQ: arithOp = '&'; break;
		}
		toker->next();
		ExprNode* rhs = parseExpr(false);
		return new CompoundAssNode(var, rhs, arithOp);
	}
	else {
		exp("assignment operator");
		return nullptr;
	}
}

std::string Parser::parseTypeTag() {
	switch (toker->curr()) {
	case '%':toker->next(); return "%";
	case '#':toker->next(); return "#";
	case '$':toker->next(); return "$";
	case '@':toker->next(); return "@";
	case '.':toker->next(); return parseIdent();
	}
	return "";
}

std::vector<std::string>* Parser::parseFuncPtrParamTags() {
	if (toker->curr() != '(') return 0;
	toker->next();
	std::unique_ptr<std::vector<std::string>> tags(new std::vector<std::string>());
	if (toker->curr() != ')') {
		for (;;) {
			std::string t;
			switch (toker->curr()) {
			case BBINT: t = "%"; toker->next(); break;
			case BBFLOAT: t = "#"; toker->next(); break;
			case BBSTR: t = "$"; toker->next(); break;
			case OBJECT: t = "@"; toker->next(); break;
			default:
				t = parseTypeTag();
				if (!t.size()) exp("parameter type");
			}
			tags->push_back(t);
			if (toker->curr() != ',') break;
			toker->next();
		}
		if (toker->curr() != ')') exp("')'");
	}
	toker->next();
	return tags.release();
}

VarNode* Parser::parseVar() {
	std::string ident = parseIdent();
	std::string tag = parseTypeTag();
	return parseVar(ident, tag);
}

VarNode* Parser::parseVar(const std::string& ident, const std::string& tag) {
	std::unique_ptr<VarNode> var;
	if (toker->curr() == '(') {
		toker->next();
		std::unique_ptr<ExprSeqNode> exprs(parseExprSeq());
		if (toker->curr() != ')') exp("')'");
		toker->next();
		var = std::unique_ptr<ArrayVarNode>(new ArrayVarNode(ident, tag, exprs.release()));
	}
	else var = std::unique_ptr<IdentVarNode>(new IdentVarNode(ident, tag));

	for (;;) {
		if (toker->curr() == '\\') {
			toker->next();
			std::string ident = parseIdent();
			std::string tag = parseTypeTag();
			ExprNode* expr = new VarExprNode(var.release());
			var = std::unique_ptr<FieldVarNode>(new FieldVarNode(expr, ident, tag));
		}
		else if (toker->curr() == '[') {
			toker->next();
			std::unique_ptr<ExprSeqNode> exprs(parseExprSeq());
			if (exprs->exprs.size() != 1 || toker->curr() != ']') exp("']'");
			toker->next();
			ExprNode* expr = new VarExprNode(var.release());
			var = std::unique_ptr<VectorVarNode>(new VectorVarNode(expr, exprs.release()));
		}
		else {
			break;
		}
	}
	return var.release();
}

DeclNode* Parser::parseVarDecl(int kind, bool constant) {
	int pos = toker->pos();
	std::string ident = parseIdent();
	std::string tag = parseTypeTag();
	DeclNode* d;
	if (experimentalSyntaxEnabled && (tag == "%" || tag == "#" || tag == "$" || tag == "@") && toker->curr() == '(') {
		std::vector<std::string>* paramTags = parseFuncPtrParamTags();
		ExprNode* expr = 0;
		if (toker->curr() == '=') {
			toker->next(); expr = parseExpr(false);
		}
		else if (constant) ex(MultiLang::constants_must_initialized);
		VarDeclNode* vd = new VarDeclNode(ident, tag, kind, constant, expr);
		vd->funcPtrParamTags = paramTags;
		funcPtrIdents[ident] = true;
		d = vd;
		d->pos = pos; d->file = incfile;
		return d;
	}
	if (toker->curr() == '[') {
		if (constant) ex(MultiLang::blitz_arrays_may_not_be_constant);
		toker->next();
		std::unique_ptr<ExprSeqNode> exprs(parseExprSeq());
		if (exprs->size() != 1 || toker->curr() != ']') exp("']'");
		toker->next();
		d = new VectorDeclNode(ident, tag, exprs.release(), kind);
	}
	else {
		ExprNode* expr = 0;
		if (toker->curr() == '=') {
			toker->next(); expr = parseExpr(false);
		}
		else if (constant) ex(MultiLang::constants_must_initialized);
		d = new VarDeclNode(ident, tag, kind, constant, expr);
	}
	d->pos = pos; d->file = incfile;
	return d;
}

DimNode* Parser::parseArrayDecl() {
	int pos = toker->pos();
	std::string ident = parseIdent();
	std::string tag = parseTypeTag();
	if (toker->curr() != '(') exp("'('");
	toker->next(); std::unique_ptr<ExprSeqNode> exprs(parseExprSeq());
	if (toker->curr() != ')') exp("')'");
	if (!exprs->size()) ex(MultiLang::cant_have_zero_dimension_array);
	toker->next();
	DimNode* d = new DimNode(ident, tag, exprs.release());
	arrayDecls[ident] = d;
	d->pos = pos;
	return d;
}

DeclNode* Parser::parseFuncDecl(bool debug) {
	int pos = toker->pos();
	std::string ident = parseIdent();
	std::string tag = parseTypeTag();
	if (toker->curr() != '(') exp("'('");
	std::unique_ptr<DeclSeqNode> params(new DeclSeqNode());
	if (toker->next() != ')') {
		for (;;) {
			params->push_back(parseVarDecl(DECL_PARAM, false));
			if (toker->curr() != ',') break;
			toker->next();
		}
		if (toker->curr() != ')') exp("')'");
	}
	toker->next();
	std::unique_ptr<StmtSeqNode> stmts(parseStmtSeq(STMTS_BLOCK, debug));
	if (toker->curr() != ENDFUNCTION) exp("'End Function'");
	StmtNode* ret = new ReturnNode(0); ret->pos = toker->pos();
	stmts->push_back(ret); toker->next();
	DeclNode* d = new FuncDeclNode(ident, tag, params.release(), stmts.release());
	d->pos = pos; d->file = incfile;
	return d;
}

DeclNode* Parser::parseStructDecl() {
	int pos = toker->pos();
	std::string ident = parseIdent();
	while (toker->curr() == '\n') toker->next();
	std::unique_ptr<DeclSeqNode> fields(new DeclSeqNode());
	while (toker->curr() == FIELD) {
		do {
			toker->next();
			fields->push_back(parseVarDecl(DECL_FIELD, false));
		} while (toker->curr() == ',');
		while (toker->curr() == '\n') toker->next();
	}
	if (toker->curr() != ENDTYPE) exp(MultiLang::field_or_end_type);
	toker->next();
	DeclNode* d = new StructDeclNode(ident, fields.release());
	d->pos = pos; d->file = incfile;
	return d;
}

DeclNode* Parser::parseEnumDecl() {
	int pos = toker->pos();
	while (toker->curr() == '\n') toker->next();
	std::string name = parseIdent();
	std::unique_ptr<EnumDeclNode> node(new EnumDeclNode(name));
	node->pos = pos;
	node->file = incfile;

	while (toker->curr() == '\n') toker->next();

	while (true) {
		if (toker->curr() == IDENT && tolower(toker->text()) == "end") {
			toker->next();
			while (toker->curr() == '\n') toker->next();
			if (toker->curr() == ENUM) {
				toker->next();
				break;
			}
			else {
				ex("'End Enum' expected");
			}
		}

		if (toker->curr() == ENDENUM) {
			toker->next();
			break;
		}

		std::string mem = parseIdent();
		ExprNode* expr = nullptr;
		if (toker->curr() == '=') {
			toker->next();
			expr = parseExpr(false);
		}
		node->members.emplace_back(mem, expr);

		while (toker->curr() == '\n' || toker->curr() == ',')
			toker->next();
	}

	return node.release();
}

IfNode* Parser::parseIf(bool debug) {
	std::unique_ptr<ExprNode> expr;
	std::unique_ptr<StmtSeqNode> stmts, elseOpt;

	expr = std::unique_ptr<ExprNode>(parseExpr(false));
	if (toker->curr() == THEN) toker->next();

	bool blkif = isTerm(toker->curr());
	stmts = std::unique_ptr<StmtSeqNode>(parseStmtSeq(blkif ? STMTS_BLOCK : STMTS_LINE, debug));

	if (toker->curr() == ELSEIF) {
		int pos = toker->pos();
		toker->next();
		IfNode* ifnode = parseIf(debug);
		ifnode->pos = pos;
		elseOpt = std::unique_ptr<StmtSeqNode>(new StmtSeqNode(incfile));
		elseOpt->push_back(ifnode);
	}
	else if (toker->curr() == ELSE) {
		toker->next();
		elseOpt = std::unique_ptr<StmtSeqNode>(parseStmtSeq(blkif ? STMTS_BLOCK : STMTS_LINE, debug));
	}
	if (blkif) {
		if (toker->curr() != ENDIF) exp("'EndIf'");
	}
	else if (toker->curr() != '\n') exp(MultiLang::end_of_file);

	return new IfNode(expr.release(), stmts.release(), elseOpt.release());
}

ExprSeqNode* Parser::parseExprSeq() {
	std::unique_ptr<ExprSeqNode> exprs(new ExprSeqNode());
	bool opt = true;
	while (ExprNode* e = parseExpr(opt)) {
		exprs->push_back(e);
		if (toker->curr() != ',') break;
		toker->next(); opt = false;
	}
	return exprs.release();
}

bool Parser::isParenList() {
	if (toker->curr() != '(') return false;
	int nest = 0;
	for (int k = 0;; ++k) {
		int t = toker->lookAhead(k);
		if (t == EOF || t == '\n' || t == ':') return false;
		if (t == '(') ++nest;
		else if (t == ')') { if (--nest == 0) return false; }
		else if (t == ',' && nest == 1) return true;
	}
}

ExprSeqNode* Parser::parseParenExprList() {
	parseChar('(');
	ExprSeqNode* exprs = parseExprSeq();
	if (toker->curr() != ')') exp("')'");
	toker->next();
	return exprs;
}

ExprNode* Parser::parseTernaryExpr() {
	parseChar(IF);
	return parseTernaryTail();
}

ExprNode* Parser::parseTernaryTail() {
	std::unique_ptr<ExprNode> cond(parseExpr(false));
	if (toker->curr() != THEN) exp("'Then'");
	toker->next();
	std::unique_ptr<ExprNode> thenExpr(parseExpr(false));
	std::unique_ptr<ExprNode> elseExpr;
	if (toker->curr() == ELSEIF) {
		toker->next();
		elseExpr = std::unique_ptr<ExprNode>(parseTernaryTail());
	}
	else if (toker->curr() == ELSE) {
		toker->next();
		elseExpr = std::unique_ptr<ExprNode>(parseExpr(false));
	}
	else exp("'Else'");
	return new TernaryExprNode(cond.release(), thenExpr.release(), elseExpr.release());
}

ExprNode* Parser::parseSelectExpr() {
	parseChar(SELECT);
	std::unique_ptr<SelectExprNode> node(new SelectExprNode(parseExpr(false)));
	for (;;) {
		while (toker->curr() == ':' || toker->curr() == '\n') toker->next();
		if (toker->curr() == CASE) {
			toker->next();
			std::unique_ptr<ExprSeqNode> exprs(parseExprSeq());
			if (!exprs->size()) exp(MultiLang::expression_sequence);
			if (toker->curr() != THEN) exp("'Then'");
			toker->next();
			node->push_back(exprs.release(), parseExpr(false));
			continue;
		}
		if (toker->curr() == DEFAULT) {
			toker->next();
			node->defaultExpr = parseExpr(false);
			break;
		}
		exp("'Case' or 'Default'");
	}
	return node.release();
}

ExprNode* Parser::parseExpr(bool opt) {
	if (toker->curr() == NOT) {
		toker->next();
		ExprNode* expr = parseExpr1(false);
		return new RelExprNode('=', expr, new IntConstNode(0));
	}
	return parseExpr1(opt);
}

ExprNode* Parser::parseExpr1(bool opt) {
	std::unique_ptr<ExprNode> lhs(parseExpr1AND(opt));
	if (!lhs) return 0;
	for (;;) {
		int c = toker->curr();
		if (c != OR && c != LOR && c != XOR) return lhs.release();
		toker->next(); ExprNode* rhs = parseExpr1AND(false);
		lhs = std::unique_ptr<ExprNode>(new BinExprNode(c, lhs.release(), rhs));
	}
}

ExprNode* Parser::parseExpr1AND(bool opt) {
	std::unique_ptr<ExprNode> lhs(parseExpr2(opt));
	if (!lhs) return 0;
	for (;;) {
		int c = toker->curr();
		if (c != AND) return lhs.release();
		toker->next(); ExprNode* rhs = parseExpr2(false);
		lhs = std::unique_ptr<ExprNode>(new BinExprNode(c, lhs.release(), rhs));
	}
}

ExprNode* Parser::parseExpr2(bool opt) {
	std::unique_ptr<ExprNode> lhs(parseExpr3(opt));
	if (!lhs) return 0;
	for (;;) {
		int c = toker->curr();
		if (c != '<' && c != '>' && c != '=' && c != LE && c != GE && c != NE
			&& !(experimentalSyntaxEnabled && (c == IS || c == ISNOT))
		) return lhs.release();

		toker->next();

		if (experimentalSyntaxEnabled && (c == IS || c == ISNOT) && toker->curr() == '(' && isParenList()) {
			std::unique_ptr<ExprSeqNode> list(parseParenExprList());
			lhs = std::unique_ptr<ExprNode>(new IsExprNode(lhs.release(), list.release(), c == ISNOT));
			continue;
		}

		ExprNode* rhs = parseExpr3(false);
		
		if (experimentalSyntaxEnabled && c == IS) {
			lhs = std::unique_ptr<ExprNode>(new RelExprNode('=', lhs.release(), rhs));
			continue;
		}
		if (experimentalSyntaxEnabled && c == ISNOT) {
			lhs = std::unique_ptr<ExprNode>(new RelExprNode(NE, lhs.release(), rhs));
			continue;
		}
		
		lhs = std::unique_ptr<ExprNode>(new RelExprNode(c, lhs.release(), rhs));
	}
}

ExprNode* Parser::parseExpr3(bool opt) {
	std::unique_ptr<ExprNode> lhs(parseExpr4(opt));
	if (!lhs) return 0;
	for (;;) {
		int c = toker->curr();
		if (c != '+' && c != '-') return lhs.release();
		toker->next(); ExprNode* rhs = parseExpr4(false);
		lhs = std::unique_ptr<ExprNode>(new ArithExprNode(c, lhs.release(), rhs));
	}
}

ExprNode* Parser::parseExpr4(bool opt) {
	std::unique_ptr<ExprNode> lhs(parseExpr5(opt));
	if (!lhs) return 0;
	for (;;) {
		int c = toker->curr();
		if (c != SHL && c != SHR && c != SAR) return lhs.release();
		toker->next(); ExprNode* rhs = parseExpr5(false);
		lhs = std::unique_ptr<ExprNode>(new BinExprNode(c, lhs.release(), rhs));
	}
}

ExprNode* Parser::parseExpr5(bool opt) {
	std::unique_ptr<ExprNode> lhs(parseExpr6(opt));
	if (!lhs) return 0;
	for (;;) {
		int c = toker->curr();
		if (c != '*' && c != '/' && c != MOD) return lhs.release();
		toker->next(); ExprNode* rhs = parseExpr6(false);
		lhs = std::unique_ptr<ExprNode>(new ArithExprNode(c, lhs.release(), rhs));
	}
}

ExprNode* Parser::parseExpr6(bool opt) {
	std::unique_ptr<ExprNode> lhs(parseUniExpr(opt));
	if (!lhs) return 0;
	for (;;) {
		int c = toker->curr();
		if (c != '^') return lhs.release();
		toker->next(); ExprNode* rhs = parseUniExpr(false);
		lhs = std::unique_ptr<ExprNode>(new ArithExprNode(c, lhs.release(), rhs));
	}
}

ExprNode* Parser::parseUniExpr(bool opt) {
	ExprNode* result = 0;
	std::string t;

	int c = toker->curr();
	switch (c) {
	case BBINT:
		if (toker->next() == '%') toker->next();
		result = parseUniExpr(false);
		result = new CastNode(result, Type::int_type);
		break;
	case BBFLOAT:
		if (toker->next() == '#') toker->next();
		result = parseUniExpr(false);
		result = new CastNode(result, Type::float_type);
		break;
	case BBSTR:
		if (toker->next() == '$') toker->next();
		result = parseUniExpr(false);
		result = new CastNode(result, Type::string_type);
		break;
	case OBJECT:
		if (toker->next() == '.') toker->next();
		t = parseIdent();
		result = parseUniExpr(false);
		result = new ObjectCastNode(result, t);
		break;
	case BBHANDLE:
		toker->next();
		result = parseUniExpr(false);
		result = new ObjectHandleNode(result);
		break;
	case ADDROF: {
		toker->next();
		VarNode* v = parseVar();
		result = new AddrOfNode(v);
		break;
	}
	case OFFSETOF: {
		toker->next();
		if (toker->curr() != '(') exp("'('");
		toker->next();
		std::string typeIdent = parseIdent();
		if (toker->curr() != '\\') exp("'\\'");
		toker->next();
		std::string fieldIdent = parseIdent();
		if (toker->curr() != ')') exp("')'");
		toker->next();
		result = new OffsetOfNode(typeIdent, fieldIdent);
		break;
	}
	case BEFORE:
		toker->next();
		result = parseUniExpr(false);
		result = new BeforeNode(result);
		break;
	case AFTER:
		toker->next();
		result = parseUniExpr(false);
		result = new AfterNode(result);
		break;
	case NOT:
		toker->next();
		result = parseUniExpr(false);
		result = new RelExprNode('=', result, new IntConstNode(0));
		break;
	case '+':case '-':case '~':case ABS:case SGN:case POWTWO:
		if (toker->lookAhead(-1) == c) { toker->next(); result = new IntConstNode(1); break; }
		toker->next();
		result = parseUniExpr(false);
		if (c == '~') {
			result = new BinExprNode(XOR, result, new IntConstNode(-1));
		}
		else {
			result = new UniExprNode(c, result);
		}
		break;
	default:
		result = parsePrimary(opt);
	}
	return result;
}

ExprNode* Parser::parsePrimary(bool opt) {
	std::unique_ptr<ExprNode> expr;
	std::string t, ident, tag;
	ExprNode* result = 0;
	int n, k;

	if (experimentalSyntaxEnabled && toker->curr() == '.' && !withStack.empty()) {
		toker->next();  // mmmm yummy .
		std::string fieldIdent = parseIdent();
		std::string fieldTag = parseTypeTag();
		DeclVarNode* tempNode = withStack.back();
		VarExprNode* base = new VarExprNode(tempNode);
		FieldVarNode* fvn = new FieldVarNode(base, fieldIdent, fieldTag);
		return new VarExprNode(fvn);
	}

	int primaryToken = toker->curr();
	if (!experimentalSyntaxEnabled && isSoftKeyword(primaryToken)) primaryToken = IDENT;
	switch (primaryToken) {
	case '(':
		toker->next();
		expr = std::unique_ptr<ExprNode>(parseExpr(false));
		if (toker->curr() != ')') exp("')'");
		toker->next();
		result = expr.release();
		break;
	case IF:
		if (!experimentalSyntaxEnabled) { if (!opt) exp(MultiLang::expression); break; }
		result = parseTernaryExpr();
		break;
	case SELECT:
		if (!experimentalSyntaxEnabled) { if (!opt) exp(MultiLang::expression); break; }
		result = parseSelectExpr();
		break;
	case BBNEW:
		toker->next(); t = parseIdent();
		result = new NewNode(t);
		break;
	case FIRST:
		toker->next(); t = parseIdent();
		result = new FirstNode(t);
		break;
	case LAST:
		toker->next(); t = parseIdent();
		result = new LastNode(t);
		break;
	case NULLCONST:
		result = new NullConstNode();
		toker->next();
		break;
	case INTCONST:
		result = new IntConstNode(atoi(toker->text()));
		toker->next();
		break;
	case FLOATCONST:
		result = new FloatConstNode(atof(toker->text()));
		toker->next();
		break;
	case STRINGCONST:
		t = toker->text();
		result = new StringConstNode(t.substr(1, t.size() - 2));
		toker->next();
		break;
	case BINCONST:
		n = 0; t = toker->text();
		for (k = 1; k < t.size(); ++k) n = (n << 1) | (t[k] == '1');
		result = new IntConstNode(n);
		toker->next();
		break;
	case HEXCONST:
		n = 0; t = toker->text();
		for (k = 1; k < t.size(); ++k) n = (n << 4) | (isdigit((unsigned char)t[k]) ? t[k] & 0xf : (t[k] & 7) + 9);
		result = new IntConstNode(n);
		toker->next();
		break;
	case PI:
		result = new FloatConstNode(PI);
		toker->next(); break;
	case INFINITYCONST:
		result = new FloatConstNode(INFINITY);
		toker->next(); break;
	case BBTRUE:
		result = new IntConstNode(1);
		toker->next(); break;
	case BBFALSE:
		result = new IntConstNode(0);
		toker->next(); break;
	case IDENT:
		ident = toker->text();
		toker->next();
		if (experimentalSyntaxEnabled) {
			result = parsePrimaryIdent(ident);
		}
		else {
			tag = parseTypeTag();
			if (toker->curr() == '(' && arrayDecls.find(ident) == arrayDecls.end()) {
				//must be a func
				toker->next();
				std::unique_ptr<ExprSeqNode> exprs(parseExprSeq());
				if (toker->curr() != ')') exp("')'");
				toker->next();
				result = new CallNode(ident, tag, exprs.release());
			}
			else {
				//must be a var
				VarNode* var = parseVar(ident, tag);
				result = new VarExprNode(var);
			}
		}
		break;
	case DO:
	case LOOP:
	case WITH:
	case IS:
	case ISNOT:
		ident = tolower(toker->text());
		toker->next();
		result = parsePrimaryIdent(ident);
		break;
	case '%':
		if (toker->next() == IDENT
			|| (experimentalSyntaxEnabled && isSoftKeyword(toker->curr()))
			) {
			std::string fn = toker->curr() == IDENT ? toker->text() : tolower(toker->text());
			result = new CallPtrNode(fn);
			toker->next();
			break;
		}
		else {
			exp(MultiLang::identifier);
		}
	default:
		if (!opt) exp(MultiLang::expression);
	}
	return result;
}
