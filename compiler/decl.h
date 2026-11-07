#ifndef DECL_H
#define DECL_H

#include <unordered_map>

enum {
	DECL_FUNC = 1, DECL_ARRAY = 2, DECL_STRUCT = 4, DECL_ENUM = 128,			//NOT vars
	DECL_GLOBAL = 8, DECL_LOCAL = 16, DECL_PARAM = 32, DECL_FIELD = 64		//ARE vars
};

struct OverrideFunction {
	std::string name;
	int requiredParameters;
	int optionalParameters;
};

extern std::unordered_map<std::string, std::vector<OverrideFunction>> OverrideFunctionMap;

struct Type;
struct ConstType;

struct Decl {
	std::string name;
	Type* type;			//type
	int kind, offset;
	ConstType* defType;	//default value
	int pos = -1;
	std::string file;
	bool userDeclared = false;
	int useCount = 0;
	Decl(std::string s, Type* t, int k, ConstType* d = 0) :name(std::move(s)), type(t), kind(k), defType(d) {}
	~Decl();

	virtual void getName(char* buff);
};

struct DeclSeq {
	std::vector<Decl*> decls;
	std::unordered_map<std::string, Decl*> declMap;
	DeclSeq();
	~DeclSeq();
	Decl* findDecl(const std::string& s, int params);
	Decl* findDecl(const std::string& s);
	Decl* insertDecl(const std::string& s, Type* t, int kind, ConstType* d = 0);
	int size() { return decls.size(); }
};

#endif