/*
  BlitzPC assembler.
  This REALLY needs some work - very slow.
*/

#include "../std.h"
#include "../ex.h"
#include "assem_x86.h"
#include "../../MultiLang/MultiLang.h"

#include <iomanip>
#include <unordered_map>

typedef std::unordered_map<std::string, Inst*> InstMap;
typedef InstMap::value_type InstPair;
typedef InstMap::const_iterator InstIter;

static InstMap instMap;
static std::unordered_map<std::string, std::vector<Inst*>> instVariants;

Assem_x86::Assem_x86(std::istream& in, Module* mod) :Assem(in, mod) {

	static bool initialized = false;
	if (!initialized) {
		initialized = true;

		for (int k = 0; ; ++k) {
			const char* name = insts[k].name;
			if (!name || name[0] == 0) break;

			std::vector<Inst*> variants;
			variants.push_back(&insts[k]);
			int j = k + 1;
			while (insts[j].name == 0) {
				variants.push_back(&insts[j]);
				++j;
			}
			instVariants[name] = variants;
			k = j - 1;
		}
	}
}

static int findCC(const std::string& s) {
	if(s == "o") return 0;
	if(s == "no") return 1;
	if(s == "b" || s == "c" || s == "nae") return 2;
	if(s == "ae" || s == "nb" || s == "nc") return 3;
	if(s == "e" || s == "z") return 4;
	if(s == "ne" || s == "nz") return 5;
	if(s == "be" || s == "na") return 6;
	if(s == "a" || s == "nbe") return 7;
	if(s == "s") return 8;
	if(s == "ns") return 9;
	if(s == "p" || s == "pe") return 10;
	if(s == "ne" || s == "po") return 11;
	if(s == "l" || s == "nge") return 12;
	if(s == "ge" || s == "nl") return 13;
	if(s == "le" || s == "ng") return 14;
	if(s == "g" || s == "nle") return 15;
	return -1;
}

void Assem_x86::align(int n) {
	int pc = mod->getPC();
	int sz = (pc + (n - 1)) / n * n - pc;
	while(sz--) mod->emit(0x90);
}

void Assem_x86::emit(int n) {
#ifdef LOG
	std::clog << std::hex << (int(n) & 0xff) << std::dec << ' ';
#endif
	mod->emit(n);
}

void Assem_x86::emitw(int n) {
	emit(n);
	emit((n >> 8));
}

void Assem_x86::emitd(int n) {
	emitw(n);
	emitw(n >> 16);
}

void Assem_x86::emitImm(const Operand& o, int size) {

	if(size < 4 && o.immLabel.size()) throw Ex(MultiLang::immediate_value_cannot_by_label);

	switch(size) {
		case 1:emit(o.imm); return;
		case 2:emitw(o.imm); return;
		case 4:a_reloc(o.immLabel); emitd(o.imm); return;
	}
}

void Assem_x86::emitImm(const std::string& s, int size) {

	Operand op(s); op.parse();
	if(!(op.mode & IMM)) throw Ex(MultiLang::operand_must_be_immediate);
	emitImm(op, size);
}

void Assem_x86::r_reloc(const std::string& s) {
	if(!s.size()) return;
	mod->addReloc(s.c_str(), mod->getPC(), true);
}

void Assem_x86::a_reloc(const std::string& s) {
	if(!s.size()) return;
	mod->addReloc(s.c_str(), mod->getPC(), false);
}

void Assem_x86::assemInst(const std::string& name, const std::string& lhs, const std::string& rhs) {

	//parse operands
	Operand lop(lhs), rop(rhs);
	lop.parse(); rop.parse();

	//find instruction
	int cc = -1;
	Inst* inst = 0;

	//kludge for condition code instructions...
	if(name[0] == 'j') {
		if((cc = findCC(name.substr(1))) >= 0) {
			static Inst jCC = { "jCC",IMM,NONE,RW_RD | PLUSCC,"\x2\x0F\x80" };
			inst = &jCC;
		}
	}
	else if(name[0] == 's' && name.substr(0, 3) == "set") {
		if((cc = findCC(name.substr(3))) >= 0) {
			static Inst setCC = { "setne",R_M8,NONE,_2 | PLUSCC,"\x2\x0F\x90" };
			inst = &setCC;
		}
	}

	if(inst) {
		if(!(lop.mode & inst->lmode) || !(rop.mode & inst->rmode)) throw Ex(MultiLang::illegal_addressing_mode);
	}
	else {
		auto it = instVariants.find(name);
		if (it == instVariants.end())
			throw Ex(MultiLang::unrecognized_instruction);

		inst = nullptr;
		for (Inst* candidate : it->second) {
			if ((lop.mode & candidate->lmode) && (rop.mode & candidate->rmode)) {
				inst = candidate;
				break;
			}
		}
		if (!inst)
			throw Ex(MultiLang::illegal_addressing_mode);
	}

	//16/32 bit modifier - NOP for now
	if(inst->flags & (O16 | O32)) {}

	int k, n = inst->bytes[0];
	for(k = 1; k < n; ++k) emit(inst->bytes[k]);
	if(inst->flags & PLUSREG) emit(inst->bytes[k] + lop.reg);
	else if(inst->flags & PLUSCC) emit(inst->bytes[k] + cc);
	else emit(inst->bytes[k]);

	if(inst->flags & (_0 | _1 | _2 | _3 | _4 | _5 | _6 | _7 | _R)) {

		//find the memop;
		const Operand& mop =
			(inst->rmode & (MEM | MEM8 | MEM16 | MEM32 | R_M | R_M8 | R_M16 | R_M32)) ? rop : lop;

		//find the spare field value.
		int rm = 0;
		switch(inst->flags & (_0 | _1 | _2 | _3 | _4 | _5 | _6 | _7 | _R)) {
			case _0:rm = 0; break; case _1:rm = 1; break; case _2:rm = 2; break; case _3:rm = 3; break;
			case _4:rm = 4; break; case _5:rm = 5; break; case _6:rm = 6; break; case _7:rm = 7; break;
			case _R:rm = (inst->rmode & (REG8 | REG16 | REG32)) ? rop.reg : lop.reg; break;
		}
		rm <<= 3;
		if(mop.mode & REG) {			//reg
			emit(0xc0 | rm | mop.reg);
		}
		else if(mop.baseReg >= 0) {		//base, index?
			int mod = mop.offset ? 0x40 : 0x00;
			if(mop.baseLabel.size() || mop.offset < -128 || mop.offset>127) mod = 0x80;
			if(mop.baseReg == 5 && !mod) mod = 0x40;
			if(mop.indexReg >= 0) {		//base, index!
				emit(mod | rm | 4);
				emit((mop.shift << 6) | (mop.indexReg << 3) | mop.baseReg);
			}
			else {						//base, no index!
				if(mop.baseReg != 4) emit(mod | rm | mop.baseReg);
				else {
					emit(mod | rm | 4); emit((4 << 3) | mop.baseReg);
				}
			}
			if((mod & 0xc0) == 0x40) emit(mop.offset);
			else if((mod & 0xc0) == 0x80) {
				//reloc
				a_reloc(mop.baseLabel); emitd(mop.offset);
			}
		}
		else if(mop.indexReg >= 0) {	//index, no base!
			emit(rm | 4);
			emit((mop.shift << 6) | (mop.indexReg << 3) | 5);
			//reloc
			a_reloc(mop.baseLabel); emitd(mop.offset);
		}
		else {							//[disp]
			emit(rm | 5);
			//reloc
			a_reloc(mop.baseLabel); emitd(mop.offset);
		}
	}

	if(inst->flags & RW_RD) {
		r_reloc(lop.immLabel); emitd(lop.imm - 4);
	}

	if(inst->flags & IB) {
		if(lop.mode & IMM) emitImm(lop, 1); else emitImm(rop, 1);
	}
	else if(inst->flags & IW) {
		if(lop.mode & IMM) emitImm(lop, 2); else emitImm(rop, 2);
	}
	else if(inst->flags & ID) {
		if(lop.mode & IMM) emitImm(lop, 4); else emitImm(rop, 4);
	}
}

void Assem_x86::assemDir(const std::string& name, const std::string& op) {

	if(!op.size()) throw Ex(MultiLang::operand_error);

	if(name == ".db") {
		if(op[0] != '\"') emitImm(op, 1);
		else {
			if(op.size() < 2 || op[op.size() - 1] != '\"') throw Ex(MultiLang::operand_error);
			for(int k = 1; k < op.size() - 1; ++k) emit(op[k]);
		}
	}
	else if(name == ".dw") {
		emitImm(op, 2);
	}
	else if(name == ".dd") {
		emitImm(op, 4);
	}
	else if(name == ".align") {
		Operand o(op); o.parse();
		if(!(o.mode & IMM)) throw Ex(MultiLang::operand_must_be_immediate);
		align(o.imm);
	}
	else {
		throw Ex(MultiLang::unrecognized_instruction);
	}
}

void Assem_x86::assemLine(const std::string& line) {

	int i = 0;
	std::string name;
	std::vector<std::string> ops;

	//label?
	if(!isspace(line[i])) {
		while(!isspace(line[i])) ++i;
		std::string lab = line.substr(0, i);
		if(!mod->addSymbol(lab.c_str(), mod->getPC())) throw Ex(MultiLang::duplicate_label);
	}

	//skip space
	while(isspace(line[i]) && line[i] != '\n') ++i;
	if(line[i] == '\n' || line[i] == ';') return;

	//fetch instruction name
	int from = i; for(++i; !isspace(line[i]); ++i) {}
	name = line.substr(from, i - from);

	for(;;) {

		//skip space
		while(isspace(line[i]) && line[i] != '\n') ++i;
		if(line[i] == '\n' || line[i] == ';') break;

		int from = i;
		if(line[i] == '\"') {
			for(++i; line[i] != '\"' && line[i] != '\n'; ++i) {}
			if(line[i++] != '\"') throw Ex(MultiLang::missing_close_quote);
		}
		else {
			for(++i; line[i] != ',' && line[i] != ';' && line[i] != '\n'; ++i) {}
		}

		//back-up over space
		while(i && isspace(line[i - 1])) --i;
		ops.push_back(line.substr(from, i - from));

		//skip space
		while(isspace(line[i]) && line[i] != '\n') ++i;
		if(line[i] == '\n' || line[i] == ';') break;

		if(line[i++] != ',') throw Ex(MultiLang::expect_comma);
	}

	//pseudo op?
	if(name[0] == '.') {
		for(int k = 0; k < ops.size(); ++k) assemDir(name, ops[k]);
		return;
	}

	//normal instruction!
	if(ops.size() > 2) throw Ex(MultiLang::too_many_operands);
	ops.push_back(""); ops.push_back("");
	assemInst(name, ops[0], ops[1]);
}

void Assem_x86::assemble() {
	std::string buffer((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (buffer.empty()) return;

	const char* p = buffer.data();
	const char* end = p + buffer.size();
	int lineNum = 0;

	while (p < end) {
		const char* lineStart = p;
		while (p < end && *p != '\n' && *p != '\r') ++p;
		const char* lineEnd = p;

		if (p < end && *p == '\r') ++p;
		if (p < end && *p == '\n') ++p;

		std::string line(lineStart, lineEnd - lineStart);
		line += '\n';
		lineNum++;
		try {
			assemLine(line);
		}
		catch(Ex& x) {
			throw Ex(line + x.ex, x.pos, x.file);
		}
	}
}