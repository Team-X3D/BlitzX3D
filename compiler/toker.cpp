#include "std.h"
#include "toker.h"
#include "ex.h"
#include "preprocessor.h"
#include "../MultiLang/MultiLang.h"
#include <cctype>
#include <ctime>
#include <cstdio>
#include <regex>
#include <chrono>
#include <functional>
#include <unordered_map>

int Toker::chars_toked;

extern bool experimentalSyntaxEnabled;

std::map<std::string, std::string> MacroDefines;

static std::unordered_map<std::string, int> alphaTokes, lowerTokes;

static void makeKeywords()
{
    static bool made, madeExperimental;
    if (made && madeExperimental == experimentalSyntaxEnabled) return;
    made = true;
    madeExperimental = experimentalSyntaxEnabled;

    alphaTokes.clear();
    lowerTokes.clear();

    alphaTokes["Dim"] = DIM;
    alphaTokes["Goto"] = GOTO;
    alphaTokes["Gosub"] = GOSUB;
    alphaTokes["Return"] = RETURN;
    alphaTokes["Exit"] = EXIT;
    alphaTokes["If"] = IF;
    alphaTokes["Then"] = THEN;
    alphaTokes["Else"] = ELSE;
    alphaTokes["EndIf"] = ENDIF;
    alphaTokes["End If"] = ENDIF;
    alphaTokes["ElseIf"] = ELSEIF;
    alphaTokes["Else If"] = ELSEIF;
    alphaTokes["While"] = WHILE;
    alphaTokes["Wend"] = WEND;
    alphaTokes["For"] = FOR;
    alphaTokes["To"] = TO;
    alphaTokes["Step"] = STEP;
    alphaTokes["Next"] = NEXT;
    alphaTokes["Continue"] = CONTINUE;
    alphaTokes["Function"] = FUNCTION;
    alphaTokes["End Function"] = ENDFUNCTION;
    alphaTokes["Type"] = TYPE;
    alphaTokes["End Type"] = ENDTYPE;
    alphaTokes["Each"] = EACH;
    alphaTokes["Local"] = LOCAL;
    alphaTokes["Global"] = GLOBAL;
    alphaTokes["Field"] = FIELD;
    alphaTokes["Const"] = BBCONST;
    alphaTokes["Select"] = SELECT;
    alphaTokes["Case"] = CASE;
    alphaTokes["Default"] = DEFAULT;
    alphaTokes["End Select"] = ENDSELECT;
    alphaTokes["Repeat"] = REPEAT;
    alphaTokes["Until"] = UNTIL;
    alphaTokes["Forever"] = FOREVER;
    alphaTokes["Data"] = DATA;
    alphaTokes["Read"] = READ;
    alphaTokes["Restore"] = RESTORE;
    alphaTokes["Abs"] = ABS;
    alphaTokes["Sgn"] = SGN;
    alphaTokes["Mod"] = MOD;
    alphaTokes["Pi"] = PI;
    alphaTokes["True"] = BBTRUE;
    alphaTokes["False"] = BBFALSE;
    alphaTokes["Int"] = BBINT;
    alphaTokes["Float"] = BBFLOAT;
    alphaTokes["Str"] = BBSTR;
    alphaTokes["Include"] = INCLUDE;

    alphaTokes["Enum"] = ENUM;
    alphaTokes["End Enum"] = ENDENUM;

    alphaTokes["New"] = BBNEW;
    alphaTokes["Delete"] = BBDELETE;
    alphaTokes["First"] = FIRST;
    alphaTokes["Last"] = LAST;
    alphaTokes["Insert"] = INSERT;
    alphaTokes["Before"] = BEFORE;
    alphaTokes["After"] = AFTER;
    alphaTokes["Object"] = OBJECT;
    alphaTokes["Handle"] = BBHANDLE;
    alphaTokes["AddressOf"] = ADDROF;
    alphaTokes["OffsetOf"] = OFFSETOF;

    alphaTokes["And"] = AND;
    alphaTokes["Or"] = OR;
    alphaTokes["Lor"] = LOR;
    alphaTokes["Xor"] = XOR;
    alphaTokes["Not"] = NOT;
    alphaTokes["Shl"] = SHL;
    alphaTokes["Shr"] = SHR;
    alphaTokes["Sar"] = SAR;

    alphaTokes["Null"] = NULLCONST;
    alphaTokes["Infinity"] = INFINITYCONST;
    alphaTokes["PowTwo"] = POWTWO;

    if (experimentalSyntaxEnabled) {
        alphaTokes["Switch"] = SELECT;
        alphaTokes["End Switch"] = ENDSELECT;
        alphaTokes["Array"] = DIM;
        alphaTokes["Interger"] = BBINT;
        alphaTokes["Method"] = FUNCTION;
        alphaTokes["Fun"] = FUNCTION;
        alphaTokes["End Method"] = ENDFUNCTION;
        alphaTokes["EndFun"] = ENDFUNCTION;
        alphaTokes["End Fun"] = ENDFUNCTION;
        alphaTokes["EndType"] = ENDTYPE;
        alphaTokes["EndSwitch"] = ENDSELECT;
        alphaTokes["EndSel"] = ENDSELECT;
        alphaTokes["Final"] = BBCONST;
        alphaTokes["End While"] = WEND;

        alphaTokes["Do"] = DO;
        alphaTokes["Loop"] = LOOP;
        alphaTokes["With"] = WITH;
        alphaTokes["End With"] = ENDWITH;
        alphaTokes["Is"] = IS;
        alphaTokes["IsNot"] = ISNOT;
        alphaTokes["IsNaht"] = ISNOT;
        alphaTokes["Case Else"] = DEFAULT;
    }

    std::unordered_map<std::string, int>::const_iterator it;
    for (it = alphaTokes.begin(); it != alphaTokes.end(); ++it) {
        lowerTokes[tolower(it->first)] = it->second;
    }
}

Toker::Toker(const std::string& file, std::istream& in, bool debug) :inc_file(file), in(in), curr_row(-1), skipLine(false), noMacro(false)
{
    MacroDefines["__DEBUG__"] = debug ? "True" : "False";
    MacroDefines["__VERSION__"] = BASE_VER;
    makeKeywords();
    nextline();
}

std::unordered_map<std::string, int>& Toker::getKeywords()
{
    makeKeywords();
    return alphaTokes;
}

int Toker::pos()
{
    int from = (curr_toke >= 0 && curr_toke < (int)tokes.size()) ? tokes[curr_toke].from : 0;
    return ((curr_row) << 16) | from;
}

int Toker::curr()
{
    return tokes[curr_toke].n;
}

std::string Toker::text()
{
    int from = tokes[curr_toke].from, to = tokes[curr_toke].to;
    return line.substr(from, to - from);
}

int Toker::lookAhead(int n)
{
    int i = curr_toke + n;
    if (i < 0 || i >= (int)tokes.size()) return '\n';
    return tokes[i].n;
}

void Toker::nextline()
{
    ++curr_row;
    curr_toke = 0;
    tokes.clear();
    if (in.eof())
    {
        conditionalStack.clear();
        skipLine = false;

        line.resize(1); line[0] = EOF;
        tokes.push_back(Toke(EOF, 0, 1));
        return;
    }

    std::string rawLine;
    if (!getline(in, rawLine)) {
        line = "\n";
        tokes.push_back(Toke('\n', 0, 1));
        return;
    }

    if (experimentalSyntaxEnabled) {
        while (true) {
            size_t end = rawLine.find_last_not_of(" \t");
            if (end != std::string::npos) {
                rawLine.erase(end + 1);
            }
            else {
                rawLine.clear();
            }

            if (rawLine.empty() || rawLine.back() != '_') {
                break;
            }
            if (rawLine.size() > 1) {
                char prev = rawLine[rawLine.size() - 2];
                if (isalnum((unsigned char)prev) || prev == '_' || prev == '\'') {
                    break;
                }
            }

            rawLine.pop_back();

            std::string nextLine;
            if (!getline(in, nextLine)) {
                break;
            }
            ++curr_row;
            rawLine += nextLine;
        }
    }

    line = rawLine + '\n';
    chars_toked += line.size();

    if (!noMacro) {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm lt{};
        localtime_s(&lt, &in_time_t);
        static const char* months[12] = { "Jan","Feb","Mar","Apr","May","Jun", "Jul","Aug","Sep","Oct","Nov","Dec" };
        char dateBuf[32], timeBuf[32];
        std::snprintf(dateBuf, sizeof(dateBuf), "\"%02d %s %d\"", lt.tm_mday, months[lt.tm_mon >= 0 && lt.tm_mon < 12 ? lt.tm_mon : 0], 1900 + lt.tm_year);
        std::snprintf(timeBuf, sizeof(timeBuf), "\"%02d:%02d:%02d\"", lt.tm_hour, lt.tm_min, lt.tm_sec);
        MacroDefines["__DATE__"] = dateBuf;
        MacroDefines["__TIME__"] = timeBuf;
        MacroDefines["__LINE__"] = std::to_string(curr_row + 1);

        // Test and strip leading whitespace's for preprocessor directives only
        size_t firstNonWhitespace = line.find_first_not_of(" \t");
        if (firstNonWhitespace != std::string::npos)
        {
            std::string strippedLine = line.substr(firstNonWhitespace);
            if (strippedLine.starts_with("#"))
            {
                line = strippedLine;
            }
        }

        if (line.starts_with("#")) // Only parse when a Preprocessor directive has been found with #
        {
            if (line == "#pragma no_macro\n") {
                noMacro = true;
                tokes.push_back(Toke('\n', 0, 1));
                return;
            }

            if (line.starts_with("#pragma message ")) {
                std::string message = line.substr(16);
                message.pop_back();
                if (message.starts_with('"') && message.ends_with('"')) {
                    message.pop_back();
                    std::cout << "[Message] " << message.substr(1) << std::endl;
                    tokes.push_back(Toke('\n', 0, 1));
                    return;
                }
                throw Ex(MultiLang::invalid_macro_string, pos(), inc_file);
            }

            if (line.starts_with("#error "))
            {
                if (!skipLine) {
                    std::string message = line.substr(7);
                    message.pop_back();
                    if (message.starts_with('"') && message.ends_with('"')) {
                        message.pop_back();
                        throw Ex(message.substr(1), pos(), inc_file);
                    }
                    throw Ex(MultiLang::invalid_macro_string, pos(), inc_file);
                }
            }

            if (line.starts_with("#define"))
            {
                if (!skipLine)
                {
                    std::string define = line.substr(8);
                    std::string name, content;
                    int sep = define.find(' ');
                    if (sep != std::string::npos)
                    {
                        name = define.substr(0, sep);
                        content = define.substr(sep + 1);
                        content.pop_back();
                    }
                    else
                    {
                        name = define;
                        name.pop_back();
                        content = "1";
                    }
                    if (!isValidIdentifier(name)) throw Ex(std::format(MultiLang::invalid_macro_name, name), pos(), inc_file);
                    if (MacroDefines.contains(name)) throw Ex(std::format(MultiLang::redefinition_of_macro, name), pos(), inc_file);
                    MacroDefines[name] = content;
                }
                tokes.push_back(Toke('\n', 0, 1));
                return;
            }

            if (line.starts_with("#undef"))
            {
                if (!skipLine)
                {
                    std::string name = line.substr(7);
                    name.pop_back();
                    if (!isValidIdentifier(name)) throw Ex(std::format(MultiLang::invalid_macro_name, name), pos(), inc_file);
                    MacroDefines.erase(name);
                }
                tokes.push_back(Toke('\n', 0, 1));
                return;
            }

            if (line.starts_with("#ifdef")) {
                std::string name = line.substr(7);
                name.pop_back();
                bool condition = MacroDefines.contains(name);

                conditionalStack.push_back({ condition, condition });
                skipLine = !condition;

                tokes.push_back(Toke('\n', 0, 1));
                return;
            }

            if (line.starts_with("#ifndef")) {
                std::string name = line.substr(8);
                name.pop_back();
                bool condition = !MacroDefines.contains(name);

                conditionalStack.push_back({ condition, condition });
                skipLine = !condition;

                tokes.push_back(Toke('\n', 0, 1));
                return;
            }

            if (line.starts_with("#if_")) {
                std::string condition = line.substr(line.find_first_of(' ') + 1);
                condition.pop_back();

                for (const auto& def : MacroDefines) {
                    size_t pos = 0;
                    while ((pos = condition.find(def.first, pos)) != std::string::npos) {
                        condition.replace(pos, def.first.length(), def.second);
                        pos += def.second.length();
                    }
                }

                try {
                    bool result = evaluateExpression(condition);
                    conditionalStack.push_back({ result, result });
                    skipLine = !result;
                }
                catch (const std::exception& e) {
                    throw Ex(std::format(MultiLang::error_evaluating_condition, e.what()), pos(), inc_file);
                    conditionalStack.push_back({ false, false });
                    skipLine = true;
                }

                tokes.push_back(Toke('\n', 0, 1));
                return;
            }

            if (line.starts_with("#elseif_")) {
                if (conditionalStack.empty()) {
                    throw Ex(MultiLang::elseif_without_if, pos(), inc_file);
                    skipLine = true;
                    tokes.push_back(Toke('\n', 0, 1));
                    return;
                }

                if (conditionalStack.back().condition_met) {
                    skipLine = true;
                    tokes.push_back(Toke('\n', 0, 1));
                    return;
                }

                std::string condition = line.substr(line.find_first_of(' ') + 1);
                condition.pop_back();

                for (const auto& def : MacroDefines) {
                    size_t pos = 0;
                    while ((pos = condition.find(def.first, pos)) != std::string::npos) {
                        condition.replace(pos, def.first.length(), def.second);
                        pos += def.second.length();
                    }
                }

                try {
                    bool result = evaluateExpression(condition);
                    conditionalStack.back().condition_state = result;
                    conditionalStack.back().condition_met |= result;
                    skipLine = !result;
                }
                catch (const std::exception& e) {
                    throw Ex(std::format(MultiLang::error_evaluating_condition, e.what()), pos(), inc_file);
                    skipLine = true;
                }

                tokes.push_back(Toke('\n', 0, 1));
                return;
            }

            if (line.starts_with("#else_")) {
                if (conditionalStack.empty()) {
                    throw Ex(MultiLang::else_without_if_macro, pos(), inc_file);
                    skipLine = true;
                    tokes.push_back(Toke('\n', 0, 1));
                    return;
                }

                bool shouldExecute = !conditionalStack.back().condition_met;
                conditionalStack.back().condition_state = shouldExecute;
                conditionalStack.back().condition_met |= shouldExecute;
                skipLine = !shouldExecute;

                tokes.push_back(Toke('\n', 0, 1));
                return;
            }

            if (line.starts_with("#endif_")) {
                if (!conditionalStack.empty()) {
                    conditionalStack.pop_back();
                    if (!conditionalStack.empty()) {
                        skipLine = !conditionalStack.back().condition_state;
                    }
                    else {
                        skipLine = false;
                    }
                }
                tokes.push_back(Toke('\n', 0, 1));
                return;
            }
        }

        if (skipLine)
        {
            tokes.push_back(Toke('\n', 0, 1));
            return;
        }

        static const int MAX_MACRO_DEPTH = 100;
        static int macroDepth = 0;
        if (macroDepth < MAX_MACRO_DEPTH) {
            macroDepth++;
            bool changed;
            do {
                changed = false;
                std::string newLine;
                size_t pos = 0;
                while (pos < line.size()) {
                    if (line[pos] == '"') {
                        size_t end = pos + 1;
                        while (end < line.size() && (line[end] != '"' || line[end - 1] == '\\'))
                            ++end;
                        if (end < line.size()) ++end;
                        newLine += line.substr(pos, end - pos);
                        pos = end;
                        continue;
                    }
                    if (line[pos] == ';') {
                        newLine += line.substr(pos);
                        break;
                    }
                    bool matched = false;
                    for (const auto& [name, value] : MacroDefines) {
                        if (line.compare(pos, name.size(), name) == 0 &&
                            (pos == 0 || (!isalnum((unsigned char)line[pos - 1]) && line[pos - 1] != '_')) &&
                            (pos + name.size() == line.size() || (!isalnum((unsigned char)line[pos + name.size()]) && line[pos + name.size()] != '_'))) {
                            newLine += value;
                            pos += name.size();
                            changed = true;
                            matched = true;
                            break;
                        }
                    }
                    if (!matched) {
                        newLine += line[pos];
                        ++pos;
                    }
                }
                line = std::move(newLine);
            } while (changed);
            macroDepth--;
        }
    }

    for (int k = 0; k < line.size(); )
    {
        int c = line[k], from = k;
        if (c == '\n')
        {
            tokes.push_back(Toke(c, from, ++k));
            continue;
        }
        if (isspace((unsigned char)c)) { ++k; continue; }
        if (c == ';')
        {
            for (++k; line[k] != '\n'; ++k) {}
            continue;
        }
        if (c == '.' && isdigit((unsigned char)line[k + 1]))
        {
            for (k += 2; isdigit((unsigned char)line[k]); ++k) {}
            tokes.push_back(Toke(FLOATCONST, from, k));
            continue;
        }
        if (isdigit((unsigned char)c))
        {
            for (++k; isdigit((unsigned char)line[k]); ++k) {}
            if (line[k] == '.')
            {
                for (++k; isdigit((unsigned char)line[k]); ++k) {}
                tokes.push_back(Toke(FLOATCONST, from, k));
                continue;
            }
            tokes.push_back(Toke(INTCONST, from, k));
            continue;
        }
        if (c == '%' && (line[k + 1] == '0' || line[k + 1] == '1'))
        {
            for (k += 2; line[k] == '0' || line[k] == '1'; ++k) {}
            tokes.push_back(Toke(BINCONST, from, k));
            continue;
        }
        if (c == '$' && isxdigit((unsigned char)line[k + 1]))
        {
            for (k += 2; isxdigit((unsigned char)line[k]); ++k) {}
            tokes.push_back(Toke(HEXCONST, from, k));
            continue;
        }
        if (isalpha((unsigned char)c))
        {
            for (++k; isalnum((unsigned char)line[k]) || line[k] == '_'
                || (experimentalSyntaxEnabled && line[k] == '\'')
                ; ++k) {
            }

            std::string ident = tolower(line.substr(from, k - from));

            if (line[k] == ' ' && isalpha((unsigned char)line[k + 1]))
            {
                int t = k;
                for (t += 2; isalnum((unsigned char)line[t]) || line[t] == '_'; ++t) {}
                std::string s = tolower(line.substr(from, t - from));
                if (lowerTokes.find(s) != lowerTokes.end())
                {
                    k = t; ident = s;
                }
            }

            auto it = lowerTokes.find(ident);

            if (it == lowerTokes.end())
            {
                for (int n = from; n < k; ++n) line[n] = tolower((unsigned char)line[n]);
                tokes.push_back(Toke(IDENT, from, k));
                continue;
            }

            tokes.push_back(Toke(it->second, from, k));
            continue;
        }
        if (c == '\"')
        {
            for (++k; line[k] != '\"' && line[k] != '\n'; ++k) {}
            if (line[k] == '\"') ++k;
            tokes.push_back(Toke(STRINGCONST, from, k));
            continue;
        }
        int n = line[k + 1];
        if ((c == '<' && n == '>') || (c == '>' && n == '<') || (c == '!' && n == '='))
        {
            tokes.push_back(Toke(NE, from, k += 2));
            continue;
        }
        if ((c == '<' && n == '=') || (c == '=' && n == '<'))
        {
            tokes.push_back(Toke(LE, from, k += 2));
            continue;
        }
        if ((c == '>' && n == '=') || (c == '=' && n == '>'))
        {
            tokes.push_back(Toke(GE, from, k += 2));
            continue;
        }
        if (experimentalSyntaxEnabled && (c == '+' || c == '-' || c == '*' || c == '/' || c == '&') && line[k + 1] == '=') {
            int token = 0;
            switch (c) {
            case '+': token = PLUSEQ; break;
            case '-': token = MINUSEQ; break;
            case '*': token = STAREQ; break;
            case '/': token = SLASHEQ; break;
            case '&': token = AMPEQ; break;
            }
            tokes.push_back(Toke(token, from, k += 2));
            continue;
        }
        //Modern logical operators: &, |, !, !=
        if (c == '&') {
            if (n != ' ') line = line.insert(k, 1, ' ');
            tokes.push_back(Toke(AND, from, k += 2));
            continue;
        }
        if (c == '|' && n != '|') {
            if (n != ' ') line = line.insert(k, 1, ' ');
            tokes.push_back(Toke(OR, from, k += 2));
            continue;
        }
        if (c == '|' && n == '|') {
            if (line[k + 2] != ' ') line = line.insert(k + 2, 1, ' ');
            tokes.push_back(Toke(LOR, from, k += 2));
            continue;
        }
        if (c == '!') {
            if (n != ' ') line = line.insert(k, 1, ' ');
            tokes.push_back(Toke(NOT, from, k += 2));
            continue;
        }
        tokes.push_back(Toke(c, from, ++k));
    }
    if (tokes.empty()) tokes.push_back(Toke('\n', 0, 1));
}

int Toker::next()
{
    if (++curr_toke == tokes.size()) nextline();
    return curr();
}

bool Toker::isValidIdentifier(const std::string& str) {
    if (str.empty() || !isalpha((unsigned char)str[0])) return false;
    for (char c : str) {
        if (!isalnum((unsigned char)c) && c != '_' && c != '(' && c != ')') return false;
    }
    return true;
}