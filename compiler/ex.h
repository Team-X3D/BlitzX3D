#ifndef EX_H
#define EX_H

#include <string>
#include <vector>

struct Ex {
	std::string ex;		//what happened
	int pos;		//source offset
	std::string file;
	Ex(const std::string& ex) :ex(ex), pos(-1) {}
	Ex(const std::string& ex, int pos, const std::string& t) :ex(ex), pos(pos), file(t) {}
};

inline constexpr int Ex_MAX_ERRORS = 32;
extern thread_local bool Ex_collecting;
extern thread_local std::vector<Ex>* Ex_errors;

extern thread_local std::vector<Ex>* Ex_warnings;
extern thread_local std::string Ex_currentFile;

void Ex_warn(const std::string& msg, int pos, const std::string& file);
inline void Ex_warn(const std::string& msg, int pos) { Ex_warn(msg, pos, Ex_currentFile); }

struct Ex_collect_scope {
	std::vector<Ex>& out;
	Ex_collect_scope(std::vector<Ex>& o) :out(o) { Ex_collecting = true; Ex_errors = &out; }
	~Ex_collect_scope() { Ex_collecting = false; Ex_errors = nullptr; }
};

inline void Ex_fill(Ex& x, int pos, const std::string& file) {
	if (x.pos < 0) x.pos = pos;
	if (x.file.empty()) x.file = file;
}

#endif