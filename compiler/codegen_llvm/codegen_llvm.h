#ifndef CODEGEN_LLVM_H
#define CODEGEN_LLVM_H

#include "../codegen.h"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

class Codegen_llvm : public Codegen {
public:
	Codegen_llvm(std::ostream& out, bool debug);
	virtual ~Codegen_llvm();

	virtual void enter(const std::string& l, int frameSize);
	virtual void code(TNode* code);
	virtual void leave(TNode* cleanup, int pop_sz);
	virtual void label(const std::string& l);
	virtual void i_data(int i, const std::string& l);
	virtual void s_data(const std::string& s, const std::string& l);
	virtual void p_data(const std::string& p, const std::string& l);
	virtual void align_data(int n);
	virtual void flush();

private:
	enum Kind { K_INT, K_FLOAT };

	struct Val {
		std::string v;
		Kind kind;
		Val(const std::string& v = "0", Kind kind = K_INT) :v(v), kind(kind) {}
	};

	struct DataChunk {
		enum { C_INT, C_BYTES, C_PAD };
		int kind;
		int offset;
		int size;
		int ival;
		std::string ptr;
		std::string bytes;
	};

	std::set<std::string> definedFuncs;
	std::set<std::string> definedData;
	std::set<std::string> refGlobals;
	std::set<std::string> emittedGlobals;
	std::set<std::string> emittedFuncs;
	std::map<std::string, std::pair<bool, int> > externFuncs;

	std::vector<DataChunk> dataChunks;
	std::map<std::string, int> dataOffsets;
	int dataSize;

	std::set<int> usedLocals;
	std::set<int> usedParams;
	std::set<int> argSlots;
	bool frameAlloca;
	bool retvalAlloca;

	int tmpCount;
	int blockCount;

	std::set<std::string> branchTargets;
	std::set<std::string> blockLabels;

	bool inCode;
	std::string funcName;
	std::string funcBody;
	std::string funcPre;
	std::string curBlock;
	bool blockOpen;
	bool retFloat;
	bool skipNextJump;
	std::string mainReturnLabel;

	std::string ensureFuncNameStr();

	bool finalized;
	void finalize();

	void dataLabel(const std::string& l);
	void emitDataBlob();

	std::string newTmp();
	std::string newBlock();
	std::string quoteName(const std::string& s);

	void emitInstr(const std::string& s);
	void emitTerm(const std::string& s);
	void startBlock(const std::string& l);

	std::string addrGlobal(const std::string& name);
	std::string addrLocal(int offset);
	std::string addrArg(int offset);
	std::string ensureLocal(int idx);
	std::string ensureParam(int idx);
	std::string ensureFrame();
	std::string ensureRetval();

	Val munch(TNode* t);
	Val eval(TNode* t, Kind want);
	Val gen(TNode* t, Kind want);
	Val genCall(TNode* t, Kind want);
	Val genMem(TNode* t);
	Val binInt(const std::string& op, TNode* l, TNode* r);
	Val cmpInt(const std::string& pred, TNode* l, TNode* r);
	Val cmpFloat(const std::string& pred, TNode* l, TNode* r);
	Val bitUnary(const std::string& op, TNode* a);
	Val floatUnary(const std::string& op, TNode* a);
	Val floatArith(const std::string& op, TNode* l, TNode* r);

	void genStore(TNode* dst, const std::string& val);
	std::vector<TNode*> collectArgs(TNode* t, int& arity);
};

#endif
