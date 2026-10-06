#include "std.h"
#include "nodes.h"
#include "../MultiLang/MultiLang.h"
#include <math.h>
#include <float.h>
#include <format>
#include "type.h"
#include "environ.h"

static Type* commonExprType(Type* t1, Type* t2) {
	if(t1 == t2) return t1;
	if(t1 == Type::null_type) return t2;
	if(t2 == Type::null_type) return t1;
	if(t1->structType() || t2->structType()) {
		if(t1->structType() && t2->structType()) Node::ex(MultiLang::objects_types_are_different);
		Node::ex(MultiLang::illegal_operator_for_type);
	}
	if(t1->stringType() || t2->stringType()) return Type::string_type;
	if(t1->floatType() || t2->floatType()) return Type::float_type;
	return Type::int_type;
}

//////////////////////////////////
// Cast an expression to a type //
//////////////////////////////////
ExprNode* ExprNode::castTo(Type* ty, Environ* e) {
	if(!sem_type->canCastTo(ty)) {
		ex(std::format(MultiLang::illegal_type_conversion, sem_type->name(), ty->name()));
	}

	ExprNode* cast = new CastNode(this, ty);
	return cast->semant(e);
}

ExprNode* CastNode::semant(Environ* e) {
	if(!expr->sem_type) {
		expr = expr->semant(e);
	}

	if(ConstNode* c = expr->constNode()) {
		ExprNode* e;
		if(type == Type::int_type) e = new IntConstNode(c->intValue());
		else if(type == Type::float_type) e = new FloatConstNode(c->floatValue());
		else if(type->structType()) e = new NullConstNode();
		else e = new StringConstNode(c->stringValue());
		delete this;
		return e;
	}

	sem_type = type;
	return this;
}

//////////////////////////////////
// Cast an expression to a type //
//////////////////////////////////
TNode* CastNode::translate(Codegen* g) {
	TNode* t = expr->translate(g);
	if(expr->sem_type == Type::float_type && sem_type == Type::int_type) {
		//float->int
		return new TNode(IR_CAST, t, 0);
	}
	if(expr->sem_type == Type::int_type && sem_type == Type::float_type) {
		//int->float
		return new TNode(IR_FCAST, t, 0);
	}
	if(expr->sem_type == Type::string_type && sem_type == Type::int_type) {
		//str->int
		return call("__bbStrToInt", t);
	}
	if(expr->sem_type == Type::int_type && sem_type == Type::string_type) {
		//int->str
		return call("__bbStrFromInt", t);
	}
	if(expr->sem_type == Type::string_type && sem_type == Type::float_type) {
		//str->float
		return fcall("__bbStrToFloat", t);
	}
	if(expr->sem_type == Type::float_type && sem_type == Type::string_type) {
		//float->str
		return call("__bbStrFromFloat", t);
	}
	if(expr->sem_type->structType() && sem_type == Type::string_type) {
		//obj->str
		return call("__bbObjToStr", t);
	}
	return t;
}

/////////////////////////////
// Sequence of Expressions //
/////////////////////////////
void ExprSeqNode::semant(Environ* e) {
	for(int k = 0; k < exprs.size(); ++k) {
		if(exprs[k]) exprs[k] = exprs[k]->semant(e);
	}
}

TNode* ExprSeqNode::translate(Codegen* g, bool cfunc) {
	TNode* t = 0, * l = 0;
	for(int k = 0; k < exprs.size(); ++k) {

		TNode* q = exprs[k]->translate(g);

		if(cfunc) {
			Type* ty = exprs[k]->sem_type;
			if(ty->stringType()) {
				q = call("__bbStrToCStr", q);
			}
			else if(ty->structType()) {
				q = new TNode(IR_MEM, q);
			}
			else if(ty == Type::void_type) {
				q = new TNode(IR_MEM, add(q, iconst(4)));
			}
		}

		TNode* p;
		p = new TNode(IR_ARG, 0, 0, k * 4);
		p = new TNode(IR_MEM, p, 0);
		p = new TNode(IR_MOVE, q, p);
		p = new TNode(IR_SEQ, p, 0);
		if(l) l->r = p;
		else t = p;
		l = p;
	}
	return t;
}

void ExprSeqNode::castTo(DeclSeq* decls, Environ* e, bool cfunc) {
	if(exprs.size() > decls->size()) ex(MultiLang::too_many_parameters);
	for(int k = 0; k < decls->size(); ++k) {
		Decl* d = decls->decls[k];
		if(k < exprs.size() && exprs[k]) {

			if(cfunc && d->type->structType()) {
				if(exprs[k]->sem_type->structType()) {
				}
				else if(exprs[k]->sem_type->intType()) {
					exprs[k]->sem_type = Type::void_type;
				}
				else {
					ex(std::format(MultiLang::illegal_type_conversion, exprs[k]->sem_type->name(), d->type->name()));
				}
				continue;
			}

			exprs[k] = exprs[k]->castTo(d->type, e);

		}
		else {
			if(!d->defType) ex(MultiLang::not_enough_parameters);
			ExprNode* expr = constValue(d->defType);
			if(k < exprs.size()) exprs[k] = expr;
			else exprs.push_back(expr);
		}
	}
}

void ExprSeqNode::castTo(Type* t, Environ* e) {
	for(int k = 0; k < exprs.size(); ++k) {
		exprs[k] = exprs[k]->castTo(t, e);
	}
}

///////////////////
// Function call //
///////////////////
ExprNode* CallNode::semant(Environ* e) {
	Type* t = e->findType(tag);
	sem_decl = e->findFunc(ident, exprs->size());
	
	if (!sem_decl || !(sem_decl->kind & DECL_FUNC)) { 
		ex(std::format(MultiLang::function_not_found, ident));
	};
	ident = sem_decl->name;
	FuncType* f = sem_decl->type->funcType();
	if(t && f->returnType != t) ex(MultiLang::incorrect_function_return_type);
	exprs->semant(e);
	exprs->castTo(f->params, e, f->cfunc);
	sem_type = f->returnType;
	return this;
}

TNode* CallNode::translate(Codegen* g) {

	FuncType* f = sem_decl->type->funcType();

	TNode* t;
	TNode* l = global("_f" + ident);
	TNode* r = exprs->translate(g, f->cfunc);

	if(f->userlib) {
		l = new TNode(IR_MEM, l);
		usedfuncs.insert(ident);
	}

	if(sem_type == Type::float_type) {
		t = new TNode(IR_FCALL, l, r, exprs->size() * 4);
	}
	else {
		t = new TNode(IR_CALL, l, r, exprs->size() * 4);
	}

	if(f->returnType->stringType()) {
		if(f->cfunc) {
			t = call("__bbCStrToStr", t);
		}
	}
	return t;
}
///////////////////////
// Function Pointer ///
///////////////////////

ExprNode* CallPtrNode::semant(Environ* env) {
	sem_decl = env->findFunc(ident);

	if (!sem_decl || !(sem_decl->kind & DECL_FUNC)) {
		Type* t = env->findType(ident);
		if (t && t->structType()) {
			sem_isType = true;
			sem_type = Type::pointer_type;
			return this;
		}
		ex(std::format(MultiLang::function_not_found, ident));
	}
	if (OverrideFunctionMap.contains(ident)) ex(MultiLang::ambiguous_function_reference);

	if (experimentalSyntaxEnabled) {
		FuncType* f = sem_decl->type->funcType();
		std::vector<Type*> paramTypes;
		for(int k = 0; k < f->params->size(); ++k) {
			paramTypes.push_back(f->params->decls[k]->type);
		}
		sem_type = new FuncPtrType(f->returnType, paramTypes);
	}
	else {
		sem_type = Type::pointer_type;
	}
	return this;
}

TNode* CallPtrNode::translate(Codegen* g) {
	if (sem_isType) return global("_t" + ident);
	return global("_f" + ident);
}

////////////////////////////////
// Call through function ptr  //
////////////////////////////////
ExprNode* CallIndirectNode::semant(Environ* e) {
	funcExpr = funcExpr->semant(e);
	sem_fptype = funcExpr->sem_type->funcPtrType();
	if(!sem_fptype) ex("'" + funcExpr->sem_type->name() + "' is not a function pointer and cannot be called");

	if(exprs->size() > (int)sem_fptype->paramTypes.size()) ex(MultiLang::too_many_parameters);
	if(exprs->size() < (int)sem_fptype->paramTypes.size()) ex(MultiLang::not_enough_parameters);

	exprs->semant(e);
	for(int k = 0; k < exprs->size(); ++k) {
		exprs->exprs[k] = exprs->exprs[k]->castTo(sem_fptype->paramTypes[k], e);
	}

	sem_type = sem_fptype->returnType;
	return this;
}

TNode* CallIndirectNode::translate(Codegen* g) {

	TNode* l = funcExpr->translate(g);
	TNode* r = exprs->translate(g, false);

	if(sem_type == Type::float_type) {
		return new TNode(IR_FCALL, l, r, exprs->size() * 4);
	}
	return new TNode(IR_CALL, l, r, exprs->size() * 4);
}

/////////////////////////
// Variable expression //
/////////////////////////
ExprNode* VarExprNode::semant(Environ* e) {
	var->semant(e);
	sem_type = var->sem_type;
	ConstType* c = sem_type->constType();
	if(!c) return this;
	ExprNode* expr = constValue(c);
	delete this; return expr;
}

TNode* VarExprNode::translate(Codegen* g) {
	return var->load(g);
}

//////////////////////
// Integer constant //
//////////////////////
IntConstNode::IntConstNode(int n) :value(n) {
	sem_type = Type::int_type;
}

TNode* IntConstNode::translate(Codegen* g) {
	return new TNode(IR_CONST, 0, 0, value);
}

int IntConstNode::intValue() {
	return value;
}

float IntConstNode::floatValue() {
	return value;
}

std::string IntConstNode::stringValue() {
	return itoa(value);
}

///////////////////
// Null constant //
///////////////////
NullConstNode::NullConstNode() {
	sem_type = Type::null_type;
}

TNode* NullConstNode::translate(Codegen* g) {
	return new TNode(IR_CONST, 0, 0, 0);
}

int NullConstNode::intValue() {
	ex(MultiLang::cant_convert_null_to_int);
	return 0;
}

float NullConstNode::floatValue() {
	ex(MultiLang::cant_convert_null_to_float);
	return 0.f;
}

std::string NullConstNode::stringValue() {
	ex(MultiLang::cant_convert_null_to_string);
	return std::string("");
}

////////////////////
// Float constant //
////////////////////
FloatConstNode::FloatConstNode(float f) :value(f) {
	sem_type = Type::float_type;
}

TNode* FloatConstNode::translate(Codegen* g) {
	return new TNode(IR_CONST, 0, 0, *(int*)&value);
}

int FloatConstNode::intValue() {
	float flt = value;
	int temp;
	_control87(_RC_NEAR | _PC_24 | _EM_INVALID | _EM_ZERODIVIDE | _EM_OVERFLOW | _EM_UNDERFLOW | _EM_INEXACT | _EM_DENORMAL, 0xfffff);
	_asm {
		fld[flt];
		fistp[temp];
	}
	_control87(_CW_DEFAULT, 0xfffff);
	return temp;
}

float FloatConstNode::floatValue() {
	return value;
}

std::string FloatConstNode::stringValue() {
	return ftoa(value);
}

/////////////////////
// String constant //
/////////////////////
StringConstNode::StringConstNode(const std::string& s) :value(s) {
	sem_type = Type::string_type;
}

TNode* StringConstNode::translate(Codegen* g) {
	std::string lab = genLabel();
	g->s_data(value, lab);
	return call("__bbStrConst", global(lab));
}

int StringConstNode::intValue() {
	return atoi(value);
}

float StringConstNode::floatValue() {
	return (float)atof(value);
}

std::string StringConstNode::stringValue() {
	return value;
}

////////////////////
// Unary operator //
////////////////////
ExprNode* UniExprNode::semant(Environ* e) {
	expr = expr->semant(e);
	sem_type = expr->sem_type;
	if(sem_type != Type::int_type && sem_type != Type::float_type) ex(MultiLang::illegal_operator_for_type);
	if(ConstNode* c = expr->constNode()) {
		ExprNode* e;
		if(sem_type == Type::int_type) {
			switch(op) {
				case '+':e = new IntConstNode(+c->intValue()); break;
				case '-':e = new IntConstNode(-c->intValue()); break;
				case ABS:e = new IntConstNode(abs(c->intValue())); break;
				case SGN:e = new IntConstNode(c->intValue() > 0 ? 1 : (c->intValue() < 0 ? -1 : 0)); break;
				case POWTWO:e = new IntConstNode(c->intValue() * c->intValue()); break;
			}
		}
		else {
			switch(op) {
				case '+':e = new FloatConstNode(+c->floatValue()); break;
				case '-':e = new FloatConstNode(-c->floatValue()); break;
				case ABS:e = new FloatConstNode(abs(c->floatValue())); break;
				case SGN:e = new FloatConstNode(c->floatValue() > 0 ? 1 : (c->floatValue() < 0 ? -1 : 0)); break;
				case POWTWO:e = new FloatConstNode(c->floatValue() * c->floatValue()); break;
			}
		}
		delete this;
		return e;
	}
	return this;
}

TNode* UniExprNode::translate(Codegen* g) {
	int n = 0;
	TNode* l = expr->translate(g);
	if(sem_type == Type::int_type) {
		switch(op) {
			case '+':return l;
			case '-':n = IR_NEG; break;
			case ABS:n = IR_ABS; break;
			case SGN:n = IR_SGN; break;
			case POWTWO:n = IR_POWTWO;
		}
	}
	else {
		switch(op) {
			case '+':return l;
			case '-':n = IR_FNEG; break;
			case ABS:n = IR_FABS; break;
			case SGN:n = IR_FSGN; break;
			case POWTWO:n = IR_FPOWTWO;
		}
	}
	return new TNode(n, l, 0);
}

/////////////////////////////////////////////////////
// boolean expression - accepts ints, returns ints //
/////////////////////////////////////////////////////
ExprNode* BinExprNode::semant(Environ* e) {
	lhs = lhs->semant(e); lhs = lhs->castTo(Type::int_type, e);
	rhs = rhs->semant(e); rhs = rhs->castTo(Type::int_type, e);
	ConstNode* lc = lhs->constNode(), * rc;
	if(lc) {
		switch(op) {
			case AND:
				if(lc->intValue()) { //Only evaluate right expression, if left expression is not false
					rc = rhs->constNode();
					if(rc) {
						ExprNode* expr;
						expr = new IntConstNode(lc->intValue() & rc->intValue());
						delete this;
						return expr;
					}
				}
				else { //If left expression is false, the whole expression must be false
					ExprNode* expr;
					expr = new IntConstNode(lc->intValue());
					delete this;
					return expr;
				}
				break;
			case OR:
				//Short-circuit evaluation cannot be applied to bitwise OR;
				//implement logical OR with another keyword
				//It is done ~ Salvage
				rc = rhs->constNode();
				if(rc) {
					ExprNode* expr;
					expr = new IntConstNode(lc->intValue() | rc->intValue());
					delete this;
					return expr;
				}
				break;
			case LOR:
				if(!lc->intValue()) { //Only evaluate right expression, if left expression is false
					rc = rhs->constNode();
					if(rc) {
						ExprNode* expr;
						expr = new IntConstNode(lc->intValue() || rc->intValue());
						delete this;
						return expr;
					}
				}
				else { //If left expression is true, the whole expression must be true
					ExprNode* expr;
					expr = new IntConstNode(1);
					delete this;
					return expr;
				}
				break;
			case XOR:
				rc = rhs->constNode();
				if(rc) {
					ExprNode* expr;
					expr = new IntConstNode(lc->intValue() ^ rc->intValue());
					delete this;
					return expr;
				}
				break;
			case SHL:
				rc = rhs->constNode();
				if(rc) {
					ExprNode* expr;
					expr = new IntConstNode(lc->intValue() << rc->intValue());
					delete this;
					return expr;
				}
				break;
			case SHR:
				rc = rhs->constNode();
				if(rc) {
					ExprNode* expr;
					expr = new IntConstNode((unsigned)lc->intValue() >> rc->intValue());
					delete this;
					return expr;
				}
				break;
			case SAR:
				rc = rhs->constNode();
				if(rc) {
					ExprNode* expr;
					expr = new IntConstNode(lc->intValue() >> rc->intValue());
					delete this;
					return expr;
				}
				break;
		}
	}
	sem_type = Type::int_type;
	return this;
}

TNode* BinExprNode::translate(Codegen* g) {
	TNode* l = lhs->translate(g);
	TNode* r = rhs->translate(g);
	int n = 0; std::string label;
	switch(op) {
		case AND:n = IR_AND; label = genLabel(); break;
		case LOR:n = IR_LOR; label = genLabel(); break;
		case OR:n = IR_OR; break; case XOR:n = IR_XOR; break;
		case SHL:n = IR_SHL; break; case SHR:n = IR_SHR; break; case SAR:n = IR_SAR; break;
	}
	return new TNode(n, l, r, label);
}

///////////////////////////
// arithmetic expression //
///////////////////////////
ExprNode* ArithExprNode::semant(Environ* e) {
	lhs = lhs->semant(e);
	rhs = rhs->semant(e);
	if(lhs->sem_type->structType() || rhs->sem_type->structType()) {
		ex(MultiLang::arithmetic_operator_custom_type);
	}
	if(lhs->sem_type == Type::string_type || rhs->sem_type == Type::string_type) {
		//one side is a string - only + operator...
		if(op != '+') ex(MultiLang::operator_cannot_applied_to_strings);
		sem_type = Type::string_type;
	}
	else if(op == '^' || lhs->sem_type == Type::float_type || rhs->sem_type == Type::float_type) {
		//It's ^, or one side is a float
		sem_type = Type::float_type;
	}
	else {
		//must be 2 ints
		sem_type = Type::int_type;
	}
	lhs = lhs->castTo(sem_type, e);
	rhs = rhs->castTo(sem_type, e);
	ConstNode* lc = lhs->constNode(), * rc = rhs->constNode();
	if(rc && (op == '/' || op == MOD)) {
		if((sem_type == Type::int_type && !rc->intValue()) || (sem_type == Type::float_type && !rc->floatValue())) {
			ex(MultiLang::division_by_zero);
		}
	}
	if(lc && rc) {
		ExprNode* expr;
		if(sem_type == Type::string_type) {
			expr = new StringConstNode(lc->stringValue() + rc->stringValue());
		}
		else if(sem_type == Type::int_type) {
			switch(op) {
				case '+':expr = new IntConstNode(lc->intValue() + rc->intValue()); break;
				case '-':expr = new IntConstNode(lc->intValue() - rc->intValue()); break;
				case '*':expr = new IntConstNode(lc->intValue() * rc->intValue()); break;
				case '/':expr = new IntConstNode(lc->intValue() / rc->intValue()); break;
				case MOD:expr = new IntConstNode(lc->intValue() % rc->intValue()); break;
			}
		}
		else {
			switch(op) {
				case '+':expr = new FloatConstNode(lc->floatValue() + rc->floatValue()); break;
				case '-':expr = new FloatConstNode(lc->floatValue() - rc->floatValue()); break;
				case '*':expr = new FloatConstNode(lc->floatValue() * rc->floatValue()); break;
				case '/':expr = new FloatConstNode(lc->floatValue() / rc->floatValue()); break;
				case MOD:expr = new FloatConstNode(fmod(lc->floatValue(), rc->floatValue())); break;
				case '^':expr = new FloatConstNode(pow(lc->floatValue(), rc->floatValue())); break;
			}
		}
		delete this;
		return expr;
	}
	return this;
}

TNode* ArithExprNode::translate(Codegen* g) {
	TNode* l = lhs->translate(g);
	TNode* r = rhs->translate(g);
	if(sem_type == Type::string_type) {
		return call("__bbStrConcat", l, r);
	}
	int n = 0;
	if(sem_type == Type::int_type) {
		switch(op) {
			case '+':n = IR_ADD; break; case '-':n = IR_SUB; break;
			case '*':n = IR_MUL; break; case '/':n = IR_DIV; break;
			case MOD:n = IR_MOD; break;
		}
	}
	else {
		switch(op) {
			case '+':n = IR_FADD; break; case '-':n = IR_FSUB; break;
			case '*':n = IR_FMUL; break; case '/':n = IR_FDIV; break;
			case MOD:return fcall("__bbFMod", l, r);
			case '^':return fcall("__bbFPow", l, r);
		}
	}
	return new TNode(n, l, r);
}

/////////////////////////
// relation expression //
/////////////////////////
ExprNode* RelExprNode::semant(Environ* e) {
	lhs = lhs->semant(e);
	rhs = rhs->semant(e);
	if(lhs->sem_type->structType() || rhs->sem_type->structType()) {
		if(op != '=' && op != NE) ex(MultiLang::illegal_operator_for_type);
		opType = lhs->sem_type != Type::null_type ? lhs->sem_type : rhs->sem_type;
	}
	else if(lhs->sem_type == Type::string_type || rhs->sem_type == Type::string_type) {
		opType = Type::string_type;
	}
	else if(lhs->sem_type == Type::float_type || rhs->sem_type == Type::float_type) {
		opType = Type::float_type;
	}
	else {
		opType = Type::int_type;
	}
	sem_type = Type::int_type;
	lhs = lhs->castTo(opType, e);
	rhs = rhs->castTo(opType, e);
	ConstNode* lc = lhs->constNode(), * rc = rhs->constNode();
	if(lc && rc) {
		ExprNode* expr;
		if(opType == Type::string_type) {
			switch(op) {
				case '<':expr = new IntConstNode(lc->stringValue() < rc->stringValue()); break;
				case '=':expr = new IntConstNode(lc->stringValue() == rc->stringValue()); break;
				case '>':expr = new IntConstNode(lc->stringValue() > rc->stringValue()); break;
				case LE: expr = new IntConstNode(lc->stringValue() <= rc->stringValue()); break;
				case NE: expr = new IntConstNode(lc->stringValue() != rc->stringValue()); break;
				case GE: expr = new IntConstNode(lc->stringValue() >= rc->stringValue()); break;
			}
		}
		else if(opType == Type::float_type) {
			switch(op) {
				case '<':expr = new IntConstNode(lc->floatValue() < rc->floatValue()); break;
				case '=':expr = new IntConstNode(lc->floatValue() == rc->floatValue()); break;
				case '>':expr = new IntConstNode(lc->floatValue() > rc->floatValue()); break;
				case LE: expr = new IntConstNode(lc->floatValue() <= rc->floatValue()); break;
				case NE: expr = new IntConstNode(lc->floatValue() != rc->floatValue()); break;
				case GE: expr = new IntConstNode(lc->floatValue() >= rc->floatValue()); break;
			}
		}
		else {
			switch(op) {
				case '<':expr = new IntConstNode(lc->intValue() < rc->intValue()); break;
				case '=':expr = new IntConstNode(lc->intValue() == rc->intValue()); break;
				case '>':expr = new IntConstNode(lc->intValue() > rc->intValue()); break;
				case LE: expr = new IntConstNode(lc->intValue() <= rc->intValue()); break;
				case NE: expr = new IntConstNode(lc->intValue() != rc->intValue()); break;
				case GE: expr = new IntConstNode(lc->intValue() >= rc->intValue()); break;
			}
		}
		delete this;
		return expr;
	}
	return this;
}

TNode* RelExprNode::translate(Codegen* g) {
	TNode* l = lhs->translate(g);
	TNode* r = rhs->translate(g);
	return compare(op, l, r, opType);
}

////////////////////
// New expression //
////////////////////
ExprNode* NewNode::semant(Environ* e) {
	sem_type = e->findType(ident);
	if(!sem_type) ex(MultiLang::custom_type_not_found);
	if(sem_type->structType() == 0) ex(MultiLang::type_is_not_custom_type);
	return this;
}

TNode* NewNode::translate(Codegen* g) {
	return call("__bbObjNew", global("_t" + ident));
}

////////////////////
// First of class //
////////////////////
ExprNode* FirstNode::semant(Environ* e) {
	sem_type = e->findType(ident);
	if(!sem_type) ex(MultiLang::custom_type_not_found);
	return this;
}

TNode* FirstNode::translate(Codegen* g) {
	return call("__bbObjFirst", global("_t" + ident));
}

///////////////////
// Last of class //
///////////////////
ExprNode* LastNode::semant(Environ* e) {
	sem_type = e->findType(ident);
	if(!sem_type) ex(MultiLang::custom_type_not_found);
	return this;
}

TNode* LastNode::translate(Codegen* g) {
	return call("__bbObjLast", global("_t" + ident));
}

////////////////////
// Next of object //
////////////////////
ExprNode* AfterNode::semant(Environ* e) {
	expr = expr->semant(e);
	if(expr->sem_type == Type::null_type) ex(MultiLang::after_cannot_used_on_null);
	if(expr->sem_type->structType() == 0) ex(MultiLang::after_must_used_with_custom_type);
	sem_type = expr->sem_type;
	return this;
}

TNode* AfterNode::translate(Codegen* g) {
	TNode* t = expr->translate(g);
	//if(g->debug) t = jumpf(t, "__bbNullObjEx");
	return call("__bbObjNext", t);
}

////////////////////
// Prev of object //
////////////////////
ExprNode* BeforeNode::semant(Environ* e) {
	expr = expr->semant(e);
	if(expr->sem_type == Type::null_type) ex(MultiLang::before_cannot_used_with_null);
	if(expr->sem_type->structType() == 0) ex(MultiLang::before_must_used_with_custom_type);
	sem_type = expr->sem_type;
	return this;
}

TNode* BeforeNode::translate(Codegen* g) {
	TNode* t = expr->translate(g);
	//if(g->debug) t = jumpf(t, "__bbNullObjEx");
	return call("__bbObjPrev", t);
}

/////////////////
// Object cast //
/////////////////
ExprNode* ObjectCastNode::semant(Environ* e) {
	expr = expr->semant(e);
	expr = expr->castTo(Type::int_type, e);
	sem_type = e->findType(type_ident);
	if(!sem_type) ex(MultiLang::custom_type_not_found);
	if(!sem_type->structType()) ex(MultiLang::type_is_not_custom_type);
	return this;
}

TNode* ObjectCastNode::translate(Codegen* g) {
	TNode* t = expr->translate(g);
	t = call("__bbObjFromHandle", t, global("_t" + sem_type->structType()->ident));
	return t;
}

///////////////////
// Object Handle //
///////////////////
ExprNode* ObjectHandleNode::semant(Environ* e) {
	expr = expr->semant(e);
	if(!expr->sem_type->structType()) ex(MultiLang::objecthandle_must_used_with_object);
	sem_type = Type::int_type;
	return this;
}

TNode* ObjectHandleNode::translate(Codegen* g) {
	TNode* t = expr->translate(g);
	return call("__bbObjToHandle", t);
}

/////////////////
// Address of  //
/////////////////
ExprNode* AddrOfNode::semant(Environ* e) {
	var->semant(e);
	if(var->sem_type->structType()) {
		ex(MultiLang::addressof_not_supported_for_type);
	}
	sem_type = Type::int_type;
	return this;
}

TNode* AddrOfNode::translate(Codegen* g) {
	return var->translate(g);
}

/////////////////
// Offset of //
///////////////
ExprNode* OffsetOfNode::semant(Environ* e) {
	Type* t = e->findType(typeIdent);
	StructType* st = t ? t->structType() : 0;
	if (!st) ex(MultiLang::custom_type_not_found);

	Decl* field = st->fields->findDecl(fieldIdent);
	if (!field) ex(MultiLang::type_field_not_found);

	sem_offset = field->offset;
	sem_type = Type::int_type;
	return this;
}

TNode* OffsetOfNode::translate(Codegen* g) {
	return iconst(sem_offset);
}

ExprNode* TypeFieldNode::semant(Environ* e) {
	Type* t = e->findType(typeIdent);
	StructType* st = t ? t->structType() : 0;
	if (!st) ex(MultiLang::custom_type_not_found);

	Decl* field = st->fields->findDecl(fieldIdent);
	if (!field) ex(MultiLang::type_field_not_found);

	sem_index = field->offset / 4;
	sem_type = Type::int_type;
	return this;
}

TNode* TypeFieldNode::translate(Codegen* g) {
	return iconst(sem_index);
}

/////////////////////////
// Conditional (If...) //
/////////////////////////
ExprNode* TernaryExprNode::semant(Environ* e) {
	cond = cond->semant(e);
	cond = cond->castTo(Type::int_type, e);
	thenExpr = thenExpr->semant(e);
	elseExpr = elseExpr->semant(e);
	sem_type = commonExprType(thenExpr->sem_type, elseExpr->sem_type);
	thenExpr = thenExpr->castTo(sem_type, e);
	elseExpr = elseExpr->castTo(sem_type, e);
	sem_temp = genLocal(e, sem_type);
	return this;
}

TNode* TernaryExprNode::translate(Codegen* g) {
	std::string _else = genLabel(), _end = genLabel();
	g->code(jumpf(cond->translate(g), _else));
	g->code(sem_temp->store(g, thenExpr->translate(g)));
	g->code(jump(_end));
	g->label(_else);
	g->code(sem_temp->store(g, elseExpr->translate(g)));
	g->label(_end);
	return sem_temp->load(g);
}

////////////////////////
// Select expression  //
////////////////////////
ExprNode* SelectExprNode::semant(Environ* e) {
	expr = expr->semant(e);
	op_type = expr->sem_type;
	if(op_type->structType()) ex(MultiLang::select_cannot_used_with_objects);
	sem_temp = genLocal(e, op_type);

	Type* resultType = 0;
	for(size_t k = 0; k < cases.size(); ++k) {
		cases[k].exprs->semant(e);
		cases[k].exprs->castTo(op_type, e);
		cases[k].result = cases[k].result->semant(e);
		resultType = resultType ? commonExprType(resultType, cases[k].result->sem_type) : cases[k].result->sem_type;
	}
	if(defaultExpr) {
		defaultExpr = defaultExpr->semant(e);
		resultType = resultType ? commonExprType(resultType, defaultExpr->sem_type) : defaultExpr->sem_type;
	}
	if(!resultType) resultType = Type::int_type;
	for(size_t k = 0; k < cases.size(); ++k) cases[k].result = cases[k].result->castTo(resultType, e);
	if(defaultExpr) defaultExpr = defaultExpr->castTo(resultType, e);
	sem_type = resultType;
	sem_result = genLocal(e, resultType);
	return this;
}

TNode* SelectExprNode::translate(Codegen* g) {
	g->code(sem_temp->store(g, expr->translate(g)));

	std::vector<std::string> labs;
	std::string end = genLabel();

	for(size_t k = 0; k < cases.size(); ++k) {
		labs.push_back(genLabel());
		for(int j = 0; j < cases[k].exprs->size(); ++j) {
			ExprNode* m = cases[k].exprs->exprs[j];
			g->code(jumpt(compare('=', sem_temp->load(g), m->translate(g), op_type), labs.back()));
		}
	}
	if(defaultExpr) g->code(sem_result->store(g, defaultExpr->translate(g)));
	g->code(jump(end));
	for(size_t k = 0; k < cases.size(); ++k) {
		g->label(labs[k]);
		g->code(sem_result->store(g, cases[k].result->translate(g)));
		g->code(jump(end));
	}
	g->label(end);
	return sem_result->load(g);
}

////////////////////////////////////////////
// Is / IsNot against a list of choices   //
////////////////////////////////////////////
ExprNode* IsExprNode::semant(Environ* e) {
	lhs = lhs->semant(e);
	Type* ty = lhs->sem_type;
	for(int k = 0; k < exprs->size(); ++k) {
		exprs->exprs[k] = exprs->exprs[k]->semant(e);
		ty = commonExprType(ty, exprs->exprs[k]->sem_type);
	}
	op_type = ty;
	lhs = lhs->castTo(ty, e);
	for(int k = 0; k < exprs->size(); ++k) exprs->exprs[k] = exprs->exprs[k]->castTo(ty, e);
	sem_temp = genLocal(e, ty);
	sem_type = Type::int_type;
	return this;
}

TNode* IsExprNode::translate(Codegen* g) {
	g->code(sem_temp->store(g, lhs->translate(g)));
	TNode* result = 0;
	for(int k = 0; k < exprs->size(); ++k) {
		TNode* cmp = compare(negate ? NE : '=', sem_temp->load(g), exprs->exprs[k]->translate(g), op_type);
		if(!result) result = cmp;
		else if(negate) result = new TNode(IR_AND, result, cmp, genLabel());
		else result = new TNode(IR_LOR, result, cmp, genLabel());
	}
	if(!result) result = new TNode(IR_CONST, 0, 0, negate ? 1 : 0);
	return result;
}