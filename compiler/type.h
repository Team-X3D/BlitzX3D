#ifndef TYPE_H
#define TYPE_H

#include "decl.h"

struct FuncType;
struct ArrayType;
struct StructType;
struct ConstType;
struct VectorType;
struct FuncPtrType;

struct Type {
	virtual ~Type() {}

	virtual bool intType() { return 0; }
	virtual bool floatType() { return 0; }
	virtual bool stringType() { return 0; }

	virtual std::string name() { return "N/A"; }

	//casts to inherited types
	virtual FuncType* funcType() { return 0; }
	virtual ArrayType* arrayType() { return 0; }
	virtual StructType* structType() { return 0; }
	virtual ConstType* constType() { return 0; }
	virtual VectorType* vectorType() { return 0; }
	virtual FuncPtrType* funcPtrType() { return 0; }

	//operators
	virtual bool canCastTo(Type* t) { return this == t; }

	//built in types
	static Type* void_type, * int_type, * float_type, * string_type, * null_type, * pointer_type;
};

struct FuncType : public Type {
	Type* returnType;
	DeclSeq* params;
	bool userlib, cfunc;
	FuncType(Type* t, DeclSeq* p, bool ulib, bool cfn) :returnType(t), params(p), userlib(ulib), cfunc(cfn) {}
	~FuncType() { delete params; }
	FuncType* funcType() { return this; }
	std::string name() { return returnType->name() + " function"; }
};

struct ArrayType : public Type {
	Type* elementType; int dims;
	ArrayType(Type* t, int n) :elementType(t), dims(n) {}
	ArrayType* arrayType() { return this; }
	std::string name() { return elementType->name() + " array"; }
};

struct StructType : public Type {
	std::string ident;
	DeclSeq* fields;
	StructType(const std::string& i) :ident(i), fields(0) {}
	StructType(const std::string& i, DeclSeq* f) :ident(i), fields(f) {}
	~StructType() { delete fields; }
	StructType* structType() { return this; }
	virtual bool canCastTo(Type* t);
	std::string name() { return "Custom type \"" + ident + "\""; }
};

struct ConstType : public Type {
	Type* valueType;
	int intValue;
	float floatValue;
	std::string stringValue;
	ConstType(int n) :intValue(n), valueType(Type::int_type) {}
	ConstType(float n) :floatValue(n), valueType(Type::float_type) {}
	ConstType(const std::string& n) :stringValue(n), valueType(Type::string_type) {}
	ConstType() :valueType(Type::null_type) {}
	ConstType* constType() { return this; }
	std::string name() { return valueType->name() + " constant"; }
};

struct VectorType : public Type {
	std::string label;
	Type* elementType;
	std::vector<int> sizes;
	VectorType(const std::string& l, Type* t, const std::vector<int>& szs) :label(l), elementType(t), sizes(szs) {}
	VectorType* vectorType() { return this; }
	virtual bool canCastTo(Type* t);
	std::string name() { return elementType->name() + " vector"; }
};

struct FuncPtrType : public Type {
	Type* returnType;
	std::vector<Type*> paramTypes;

	FuncPtrType(Type* r, const std::vector<Type*>& p) :returnType(r), paramTypes(p) {}

	FuncPtrType* funcPtrType() { return this; }

	bool matches(FuncType* f) const {
		if(!f) return false;
		if(f->returnType != returnType) return false;
		if(f->params->size() != (int)paramTypes.size()) return false;
		for(int k = 0; k < (int)paramTypes.size(); ++k) {
			if(f->params->decls[k]->type != paramTypes[k]) return false;
		}
		return true;
	}

	bool sameShape(FuncPtrType* t) const {
		if(!t) return false;
		if(t->returnType != returnType) return false;
		if(t->paramTypes.size() != paramTypes.size()) return false;
		for(int k = 0; k < (int)paramTypes.size(); ++k) {
			if(t->paramTypes[k] != paramTypes[k]) return false;
		}
		return true;
	}

	bool canCastTo(Type* t) override {
		if(t == this) return true;
		if(FuncPtrType* f = t->funcPtrType()) return sameShape(f);
		return t == Type::int_type || t == Type::pointer_type;
	}

	std::string name() {
		std::string s = returnType->name() + " function pointer(";
		for(size_t k = 0; k < paramTypes.size(); ++k) {
			if(k) s += ", ";
			s += paramTypes[k]->name();
		}
		s += ")";
		return s;
	}
};

#endif