#include <angelscript.h>
#include <scriptbuilder/scriptbuilder.h>
#include <scriptarray/scriptarray.h>
#include <scripthelper/scripthelper.h>
#include <as_jit.h>

#include "basic.h"

#include <deque>
#include <functional>
#include <vector>

#include "bbangel_string_array_specilization.h"
#include "bbruntime.h"
#include "bbsys.h"
#include "std.h"

static asIScriptEngine* engine;
static asIScriptContext* ctx;
static asCJITCompiler* jit = nullptr;
CScriptBuilder builder;

extern void bbDebugLog(BBStr* t);

BBStr* bbGetDefaultNamespace()
{
	return new BBStr(engine->GetDefaultNamespace());
}

int bbSetDefaultNamespace(BBStr* ns)
{
	auto ret = engine->SetDefaultNamespace(ns->c_str());
	delete ns;
	return ret;
}

int bbRegisterGlobalFunction(BBStr* decl, void* ptr)
{
	auto ret = engine->RegisterGlobalFunction(decl->c_str(), asFUNCTION(ptr), asCALL_STDCALL);
	delete decl;
	return ret;
}

// This is a workaround due to asCALL_STDCALL_OBJFIRST not being a thing (https://github.com/anjo76/angelscript/issues/61).
// It relies on the fact that ALL B3D parameter and return types are 32 bit and trivial. It's incredibly illegal, but works.
struct FPtrWrapper {
	void* ptr;

	void exec0(void* a) { (reinterpret_cast<void(*)(void*)>(ptr))(a); }
	void exec1(void* a, void* b) { (reinterpret_cast<void(*)(void*, void*)>(ptr))(a, b); }
	void exec2(void* a, void* b, void* c) { (reinterpret_cast<void(*)(void*, void*, void*)>(ptr))(a, b, c); }
	void exec3(void* a, void* b, void* c, void* d) { (reinterpret_cast<void(*)(void*, void*, void*, void*)>(ptr))(a, b, c, d); }
	void exec4(void* a, void* b, void* c, void* d, void* e) { (reinterpret_cast<void(*)(void*, void*, void*, void*, void*)>(ptr))(a, b, c, d, e); }
	void exec5(void* a, void* b, void* c, void* d, void* e, void* f)
		{ (reinterpret_cast<void(*)(void*, void*, void*, void*, void*, void*)>(ptr))(a, b, c, d, e, f); }
	void exec6(void* a, void* b, void* c, void* d, void* e, void* f, void* g)
		{ (reinterpret_cast<void(*)(void*, void*, void*, void*, void*, void*, void*)>(ptr))(a, b, c, d, e, f, g); }
	void exec7(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h)
		{ (reinterpret_cast<void(*)(void*, void*, void*, void*, void*, void*, void*, void*)>(ptr))(a, b, c, d, e, f, g, h); }
	void exec8(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h, void* i)
		{ (reinterpret_cast<void(*)(void*, void*, void*, void*, void*, void*, void*, void*, void*)>(ptr))(a, b, c, d, e, f, g, h, i); }
	void exec9(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h, void* i, void* j)
		{ (reinterpret_cast<void(*)(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*)>(ptr))(a, b, c, d, e, f, g, h, i, j); }

	void* exec0_ret(void* a) { return (reinterpret_cast<void*(*)(void*)>(ptr))(a); }
	void* exec1_ret(void* a, void* b) { return (reinterpret_cast<void*(*)(void*, void*)>(ptr))(a, b); }
	void* exec2_ret(void* a, void* b, void* c) { return (reinterpret_cast<void*(*)(void*, void*, void*)>(ptr))(a, b, c); }
	void* exec3_ret(void* a, void* b, void* c, void* d) { return (reinterpret_cast<void*(*)(void*, void*, void*, void*)>(ptr))(a, b, c, d); }
	void* exec4_ret(void* a, void* b, void* c, void* d, void* e) { return (reinterpret_cast<void*(*)(void*, void*, void*, void*, void*)>(ptr))(a, b, c, d, e); }
	void* exec5_ret(void* a, void* b, void* c, void* d, void* e, void* f) { return (reinterpret_cast<void*(*)(void*, void*, void*, void*, void*, void*)>(ptr))(a, b, c, d, e, f); }
	void* exec6_ret(void* a, void* b, void* c, void* d, void* e, void* f, void* g)
		{ return (reinterpret_cast<void*(*)(void*, void*, void*, void*, void*, void*, void*)>(ptr))(a, b, c, d, e, f, g); }
	void* exec7_ret(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h)
		{ return (reinterpret_cast<void*(*)(void*, void*, void*, void*, void*, void*, void*, void*)>(ptr))(a, b, c, d, e, f, g, h); }
	void* exec8_ret(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h, void* i)
		{ return (reinterpret_cast<void*(*)(void*, void*, void*, void*, void*, void*, void*, void*, void*)>(ptr))(a, b, c, d, e, f, g, h, i); }
	void* exec9_ret(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h, void* i, void* j)
		{ return (reinterpret_cast<void*(*)(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*)>(ptr))(a, b, c, d, e, f, g, h, i, j); }
};

std::deque<FPtrWrapper> wrapper_helpers;

int bbRegisterObjectMethod(BBStr* type, BBStr* decl, void* ptr, int isLast)
{
	FPtrWrapper& wrapper = wrapper_helpers.emplace_back(ptr);

	int argCount = decl->find("()") != std::string::npos ? 0 : std::count(decl->begin(), decl->end(), ',') + 1;
	asECallConvTypes callConv = isLast ? asCALL_THISCALL_OBJLAST : asCALL_THISCALL_OBJFIRST;
	int ret;
	if (decl->starts_with("void "))
	{
		switch (argCount)
		{
		case 0: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec0_ret), callConv, &wrapper); break;
		case 1: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec1_ret), callConv, &wrapper); break;
		case 2: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec2_ret), callConv, &wrapper); break;
		case 3: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec3_ret), callConv, &wrapper); break;
		case 4: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec4_ret), callConv, &wrapper); break;
		case 5: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec5_ret), callConv, &wrapper); break;
		case 6: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec6_ret), callConv, &wrapper); break;
		case 7: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec7_ret), callConv, &wrapper); break;
		case 8: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec8_ret), callConv, &wrapper); break;
		case 9: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec9_ret), callConv, &wrapper); break;
		default: RTEX("RegisterObjectMethod: Too many parameters!");
		}
	}
	else
	{
		switch (argCount)
		{
		case 0: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec0), callConv, &wrapper); break;
		case 1: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec1), callConv, &wrapper); break;
		case 2: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec2), callConv, &wrapper); break;
		case 3: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec3), callConv, &wrapper); break;
		case 4: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec4), callConv, &wrapper); break;
		case 5: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec5), callConv, &wrapper); break;
		case 6: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec6), callConv, &wrapper); break;
		case 7: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec7), callConv, &wrapper); break;
		case 8: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec8), callConv, &wrapper); break;
		case 9: ret = engine->RegisterObjectMethod(type->c_str(), decl->c_str(), asMETHOD(FPtrWrapper, exec9), callConv, &wrapper); break;
		default: RTEX("RegisterObjectMethod: Too many parameters!");
		}
	}

	delete type;
	delete decl;
	return ret;
}

int bbRegisterGlobalProperty(BBStr* decl, void* ptr)
{
	auto ret = engine->RegisterGlobalProperty(decl->c_str(), ptr);
	delete decl;
	return ret;
}

struct BBObjFactory
{
	BBObjType* type;

	BBObj* Instantiate() const
	{
		return _bbObjNew(type);
	}

	BBObj* First() const
	{
		return _bbObjFirst(type);
	}

	BBObj* Last() const
	{
		return _bbObjLast(type);
	}
};
static std::deque<BBObjFactory> factories;

static void __cdecl objDel(BBObj* obj) {
	_bbObjDelete(obj);
}

static BBObj* __cdecl objNext(BBObj* obj) {
	return obj->next;
}

static BBObj* __cdecl objPrev(BBObj* obj) {
	return obj->prev;
}

static void __cdecl objInsAfter(BBObj* obj, BBObj* after) {
	_bbObjInsAfter(obj, after);
}

static void __cdecl objInsBefore(BBObj* obj, BBObj* after) {
	_bbObjInsBefore(obj, after);
}

static void __cdecl releaseObj(BBObj* obj)
{
	_bbObjRelease(obj);
}

static void __cdecl addRefObj(BBObj* obj)
{
	++obj->ref_cnt;
}

int bbRegisterTypeFromPtr(BBStr* bbName, BBObjType* type)
{
	std::string name = *bbName;
	delete bbName;
	auto ret = engine->RegisterObjectType(name.c_str(), 0, asOBJ_REF | asOBJ_IMPLICIT_HANDLE);
	if (ret < 0) { return ret; }
	ret = engine->RegisterObjectBehaviour(name.c_str(), asBEHAVE_ADDREF, "void f()", asFUNCTION(addRefObj), asCALL_CDECL_OBJFIRST);
	if (ret < 0) { return ret; }
	ret = engine->RegisterObjectBehaviour(name.c_str(), asBEHAVE_RELEASE, "void f()", asFUNCTION(releaseObj), asCALL_CDECL_OBJFIRST);
	if (ret < 0) { return ret; }
	BBObjFactory& factory = factories.emplace_back(type);
	ret = engine->RegisterObjectBehaviour(name.c_str(), asBEHAVE_FACTORY, (name + "@+ f()").c_str(),
		asMETHOD(BBObjFactory, Instantiate), asCALL_THISCALL_ASGLOBAL, &factory);
	if (ret < 0) { return ret; }
	ret = engine->RegisterObjectMethod(name.c_str(), "void Delete()", asFUNCTION(objDel), asCALL_CDECL_OBJFIRST);
	if (ret < 0) { return ret; }
	ret = engine->RegisterObjectMethod(name.c_str(), (name + "@+ get_Next() const property").c_str(),
		asFUNCTION(objNext), asCALL_CDECL_OBJFIRST);
	if (ret < 0) { return ret; }
	ret = engine->RegisterObjectMethod(name.c_str(), (name + "@+ get_Previous() const property").c_str(),
		asFUNCTION(objPrev), asCALL_CDECL_OBJFIRST);
	if (ret < 0) { return ret; }
	ret = engine->RegisterObjectMethod(name.c_str(), ("void InsertAfter(" + name + "@)").c_str(),
		asFUNCTION(objInsAfter), asCALL_CDECL_OBJFIRST);
	if (ret < 0) { return ret; }
	ret = engine->RegisterObjectMethod(name.c_str(), ("void InsertBefore(" + name + "@)").c_str(),
		asFUNCTION(objInsBefore), asCALL_CDECL_OBJFIRST);

	std::string currNamespace = engine->GetDefaultNamespace();
	std::string namespacedName = currNamespace.empty() ? name : currNamespace + "::" + name;
	engine->SetDefaultNamespace(namespacedName.c_str());
	engine->RegisterGlobalFunction((name + "@+ get_First() property").c_str(),
		asMETHOD(BBObjFactory, First), asCALL_THISCALL_ASGLOBAL, &factory);
	engine->RegisterGlobalFunction((name + "@+ get_Last() property").c_str(),
		asMETHOD(BBObjFactory, Last), asCALL_THISCALL_ASGLOBAL, &factory);
	engine->SetDefaultNamespace(currNamespace.c_str());

	return ret;
}

int bbRegisterType(BBStr* name)
{
	auto ret = engine->RegisterObjectType(name->c_str(), 0, asOBJ_REF | asOBJ_NOCOUNT | asOBJ_IMPLICIT_HANDLE);
	delete name;
	return ret;
}

static void* __cdecl noop(void* ptr)
{
	return ptr;
}

int bbRegisterTypeInheritance(BBStr* type, BBStr* base)
{
	std::unique_ptr<BBStr> typePtr(type), basePtr(base);

	auto ret = engine->RegisterObjectMethod(base->c_str(), (*type + "@ opCast()").c_str(), asFUNCTION(noop), asCALL_CDECL_OBJFIRST);
	if (ret < 0) return ret;

	ret = engine->RegisterObjectMethod(type->c_str(), (*base + "@ opImplCast()").c_str(), asFUNCTION(noop), asCALL_CDECL_OBJFIRST);
	if (ret < 0) return ret;

	ret = engine->RegisterObjectMethod(base->c_str(), ("const " + *type + "@ opCast() const").c_str(), asFUNCTION(noop), asCALL_CDECL_OBJFIRST);
	if (ret < 0) return ret;

	ret = engine->RegisterObjectMethod(type->c_str(), ("const " + *base + "@ opImplCast() const").c_str(), asFUNCTION(noop), asCALL_CDECL_OBJFIRST);
	return ret;
}

int bbRegisterTypeConstructor(BBStr* type, BBStr* decl, void* ptr)
{
	auto ret = engine->RegisterObjectBehaviour(type->c_str(), asBEHAVE_FACTORY, decl->c_str(),
		asFUNCTION(ptr), asCALL_STDCALL);
	delete type;
	delete decl;
	return ret;
}

int bbRegisterTypeField(BBStr* type, BBStr* decl, int index)
{
	auto ret = engine->RegisterObjectProperty(type->c_str(), decl->c_str(),
		index * sizeof(BBField), asOFFSET(BBObj, fields), true);
	delete type;
	delete decl;
	return ret;
}

int bbRegisterEnum(BBStr* name)
{
	auto ret = engine->RegisterEnum(name->c_str());
	delete name;
	return ret;
}

int bbRegisterEnumValue(BBStr* enumName, BBStr* valueName, int value)
{
	auto ret = engine->RegisterEnumValue(enumName->c_str(), valueName->c_str(), value);
	delete enumName;
	delete valueName;
	return ret;
}

int bbBeginModule(BBStr* name)
{
	auto ret = builder.StartNewModule(engine, name->c_str());
	delete name;
	return ret;
}

int bbModuleAddFile(BBStr* file)
{
	auto ret = builder.AddSectionFromFile(file->c_str());
	delete file;
	return ret;
}

int bbEndModule()
{
	return builder.BuildModule();
}

asIScriptModule* bbGetModule(BBStr* name)
{
	auto mod = engine->GetModule(name->c_str());
	delete name;
	return mod;
}

asIScriptFunction* bbGetModuleFunction(asIScriptModule* module, BBStr* func)
{
	auto f = module->GetFunctionByName(func->c_str());
	delete func;
	return f;
}

void bbFreeModule(asIScriptModule* module)
{
	module->Discard();
}

BBStr* bbGetDeclarations(asIScriptModule* module = nullptr)
{
	std::stringstream ss;

	std::function<asIScriptFunction*(int)> getFunc;

	std::string curr_ns;
	std::function checkNamespace = [&](const char* ns)
	{
		if ((ns ? ns : "") == curr_ns) return;
		if (!curr_ns.empty())
			ss << "}\n";
		if (ns && *ns)
			ss << "\nnamespace " << ns << " {\n";
		curr_ns = ns ? ns : "";
	};


	asUINT count;
	if (module)
	{
		count = module->GetFunctionCount();
		getFunc = [module](int idx) { return module->GetFunctionByIndex(idx); };
	}
	else
	{
		count = engine->GetGlobalFunctionCount();
		getFunc = [](int idx) { return engine->GetGlobalFunctionByIndex(idx); };
	}

	for (int i = 0; i < count; ++i)
	{
		asIScriptFunction* func = getFunc(i);
		checkNamespace(func->GetNamespace());
		ss << func->GetDeclaration(true, false, true);
		if (func->IsProperty()) ss << " property";
		ss << ";\n";
	}

	std::function<asITypeInfo*(int)> getType;
	if (module)
	{
		count = module->GetObjectTypeCount();
		getType = [module](int idx) { return module->GetObjectTypeByIndex(idx); };
	}
	else
	{
		count = engine->GetObjectTypeCount();
		getType = [](int idx) { return engine->GetObjectTypeByIndex(idx); };
	}

	for (int i = 0; i < count; ++i)
	{
		asITypeInfo* type = getType(i);
		checkNamespace(type->GetNamespace());
		ss << "\nclass ";
		ss << type->GetName();
		if (type->GetSubTypeCount() > 0)
		{
			ss << "<";
			for (int j = 0; j < type->GetSubTypeCount(); ++j)
			{
				if (j > 0) ss << ", ";
				ss << type->GetSubType(j)->GetName();
			}

			ss << ">";
		}
		ss << " {\n";

		for (int j = 0; j < type->GetPropertyCount(); ++j)
		{
			ss << "    " << type->GetPropertyDeclaration(j, true) << ";\n";
		}

		for (int j = 0; j < type->GetChildFuncdefCount(); ++j)
		{
			ss << "    funcdef " << type->GetChildFuncdef(j)->GetFuncdefSignature()->GetDeclaration(false) << ";\n";
		}

		for (int j = 0; j < type->GetBehaviourCount(); ++j)
		{
			asEBehaviours behaviours;
			asIScriptFunction* func = type->GetBehaviourByIndex(j, &behaviours);
			if (behaviours == asBEHAVE_FACTORY || behaviours == asBEHAVE_CONSTRUCT || behaviours == asBEHAVE_DESTRUCT)
				ss << "    " << func->GetDeclaration(false, false, true) << ";\n";
		}

		for (int j = 0; j < type->GetMethodCount(); ++j)
		{
			asIScriptFunction* func = type->GetMethodByIndex(j);
			ss << "    " << func->GetDeclaration(false, false, true);
			if (func->IsProperty()) ss << " property";
			ss << ";\n";
		}
		ss << "}\n";
	}

	if (module)
	{
		for (int i = 0; i < module->GetGlobalVarCount(); ++i)
		{
			ss << module->GetGlobalVarDeclaration(i) << ";\n";
		}
	}
	else
	{
		for (int i = 0; i < engine->GetGlobalPropertyCount(); ++i)
		{
			const char* name, * ns;
			int typeId;
			engine->GetGlobalPropertyByIndex(i, &name, &ns, &typeId);
			checkNamespace(ns);
			ss << engine->GetTypeDeclaration(typeId) << " " << name << ";\n";
		}
	}

	if (module)
	{
		count = module->GetEnumCount();
		getType = [module](int idx) { return module->GetEnumByIndex(idx); };
	}
	else
	{
		count = engine->GetEnumCount();
		getType = [](int idx) { return engine->GetEnumByIndex(idx); };
	}

	for (int i = 0; i < count; ++i)
	{
		asITypeInfo* type = getType(i);
		checkNamespace(type->GetNamespace());
		ss << "enum " << type->GetName() << " {\n";
		for (int j = 0; j < type->GetEnumValueCount(); ++j)
		{
			if (j > 0) ss << ",\n";
			ss << "    " << type->GetEnumValueByIndex(j, nullptr);
		}
		ss << "\n}\n";
	}

	checkNamespace(nullptr);

	return new BBStr(ss.str());
}

static void(*appMsgCallback)(int, int, int, BBStr*, BBStr*);

struct PendingMessage { int type, row, col; std::string section, message; };
static std::vector<PendingMessage> pendingMessages;

static void messageCallback(const asSMessageInfo* msg, void* param)
{
	bbDebugLog(new BBStr(msg->message));
	if (appMsgCallback)
		appMsgCallback(msg->type, msg->row, msg->col, new BBStr(msg->section), new BBStr(msg->message));
	else
		pendingMessages.push_back({ msg->type, msg->row, msg->col,
			msg->section ? msg->section : "", msg->message ? msg->message : "" });
}

void bbSetMessageCallback(void (*callback)(int, int, int, BBStr*, BBStr*))
{
	appMsgCallback = callback;
	if (appMsgCallback)
	{
		for (const auto& m : pendingMessages)
			appMsgCallback(m.type, m.row, m.col, new BBStr(m.section), new BBStr(m.message));
		pendingMessages.clear();
	}
}

static std::vector<std::pair<int, asDWORD>> script_args;
static std::vector<std::unique_ptr<BBStr>> script_args_strings;

void bbPrepareFunction(int argc)
{
	script_args.resize(argc);
}

void bbSetArgInt(int idx, int arg)
{
	script_args[idx] = {0, arg};
}

void bbSetArgFloat(int idx, float arg)
{
	script_args[idx] = {0, *reinterpret_cast<asDWORD*>(&arg)};
}

void bbSetArgString(int idx, BBStr* arg)
{
	script_args_strings.emplace_back(arg);
	script_args[idx] = {2, reinterpret_cast<asDWORD>(arg)};
}

void bbSetArgObj(int idx, BBObj** arg)
{
	// TODO: Incredibly prone to misuse: Not executing a function after setting an object argument will offset the ref count.
	if (*arg) ++(*arg)->ref_cnt;
	script_args[idx] = {1, reinterpret_cast<asDWORD>(*arg)};
}

void bbSetArgIntObj(int idx, asDWORD arg)
{
	script_args[idx] = {1, arg};
}

static int call_depth = 0;
static bool appExceptionPending = false;

bool angel_is_executing()
{
	return call_depth > 0;
}

int bbExecuteFunction(asIScriptFunction* func, int* returnVal)
{
	if (call_depth++ > 0)
		ctx->PushState();

	auto ret = ctx->Prepare(func);
	if (ret >= 0)
	{
		for (int i = 0; i < script_args.size(); ++i)
		{
			switch (script_args[i].first) {
			case 0:
				ret = ctx->SetArgDWord(i, script_args[i].second);
				break;
			case 1:
				ret = ctx->SetArgAddress(i, (void*)script_args[i].second);
				break;
			case 2:
				ret = ctx->SetArgObject(i, (void*)&script_args[i].second);
				break;
			}

			if (ret < 0) return ret;
		}

		ret = ctx->Execute();

		if (ret == asEXECUTION_EXCEPTION) {
			std::string string = ctx->GetExceptionString();
			bool translated = appExceptionPending;
			appExceptionPending = false;

			if (!bbCallExceptionHandler(string.c_str())) {
				auto func = ctx->GetExceptionFunction();
				bbDebugLog(new BBStr(string));
				if (appMsgCallback) appMsgCallback(-1, ctx->GetExceptionLineNumber(), 0,
					new BBStr(std::string(func->GetModuleName()) + ":" + func->GetName()), new BBStr(string));

				if (translated && call_depth == 1) {
					std::wstring message(string.begin(), string.end());
					bbruntime_panic(message.c_str());
				}
			}
		} else {
			appExceptionPending = false;
			if (ret == asEXECUTION_FINISHED && returnVal) *returnVal = ctx->GetReturnDWord();
		}
	}

	if (--call_depth > 0)
		ctx->PopState();

	script_args_strings.clear();
	return ret;
}

int bbExecuteString(BBStr* code)
{
	auto ret = ExecuteString(engine, code->c_str(), nullptr, ctx);
	delete code;
	return ret;
}

class BBStringFactory : public asIStringFactory
{
public:
	BBStringFactory() = default;

	int GetRawStringData(const void* str, char* data, asUINT* length) const override
	{
		if (str == nullptr)
			return asERROR;

		const BBStr* bbStr = *(const BBStr**)str;

		if (length)
			*length = bbStr->size();

		if (data)
			memcpy(data, bbStr->c_str(), bbStr->size());

		return asSUCCESS;
	}

	const void* GetStringConstant(const char* data, asUINT length) override
	{
		BBStr** ret = new BBStr*();
		*ret = new BBStr(data, length);
		return ret;
	}

	int ReleaseStringConstant(const void* str) override
	{
		const BBStr** ptr = (const BBStr**)str;
		delete *ptr;
		delete ptr;
		return asSUCCESS;
	}
};
static BBStringFactory stringFactory;

static void __cdecl constructString(BBStr** str)
{
	*str = new BBStr();
}

static void __cdecl constructStringCStr(BBStr** str, const char* other)
{
	*str = new BBStr(other);
}

static void __cdecl copyConstructString(BBStr** str, const BBStr*& other)
{
	*str = other ? new BBStr(*other) : new BBStr();
}

static void __cdecl destructString(BBStr** str)
{
	delete *str;
}

static const BBStr*& __cdecl assignString(const BBStr*& to, const BBStr*& from)
{
	delete to;
	to = from ? new BBStr(from->c_str()) : new BBStr();
	return to;
}

static const BBStr*& __cdecl addAssignString(const BBStr*& to, const BBStr*& from)
{
	BBStr* ret = new BBStr(from ? to ? (*to + *from) : *static_cast<const std::string*>(from) : *static_cast<const std::string*>(to));
	delete to;
	to = ret;
	return to;
}

static const BBStr* __cdecl addString(const BBStr*& a, const BBStr*& b)
{
	return new BBStr(a ? b ? (*a + *b) : *static_cast<const std::string*>(a) : *static_cast<const std::string*>(b));
}

static int __cdecl equalsString(const BBStr*& a, const BBStr*& b)
{
	return *a == *b;
}

static unsigned __cdecl lengthString(const BBStr*& str)
{
	return str->size();
}

static BBStr* __cdecl substrString(const BBStr*& str, int start, int count)
{
	return new BBStr(str->substr(start, count));
}

static int __cdecl findString(const BBStr*& str, const BBStr*& toFind, int start)
{
	auto found = str->find(*toFind, start);
	if (found == std::string::npos) { return -1; }
	return found;
}

static BBStr* __cdecl replaceString(const BBStr*& str, const BBStr*& toReplace, const BBStr*& replaceWith)
{
	BBStr* result = new BBStr(*str);
	size_t pos = 0;
	while ((pos = result->find(*toReplace, pos)) != std::string::npos)
	{
		result->replace(pos, toReplace->size(), *replaceWith);
		pos += replaceWith->size();
	}
	return result;
}

template <typename T> static T __cdecl stringToFloat(const BBStr*& str) { return atof(*str); }
template <typename T>
static T __cdecl stringToInt(const BBStr*& str) {
	if constexpr (sizeof(T) == 8)
	{
		if constexpr (std::is_unsigned_v<T>)
			return strtoull(str->c_str(), nullptr, 10);
		else
			return strtoll(str->c_str(), nullptr, 10);
	}
	else
	{
		if constexpr (std::is_unsigned_v<T>)
			return strtoul(str->c_str(), nullptr, 10);
		else
			return atoi(*str);
	}
}

BBStr* __cdecl boolToString(int b) {
	return new BBStr(b ? "true" : "false");
}

template <typename T>
BBStr* __cdecl toString(T val)
{
	return new BBStr(std::to_string(val));
}

const char* __cdecl strCStr(const BBStr*& str)
{
	return str->c_str();
}

static void arrayIndexError()
{
	if (asIScriptContext* active = asGetActiveContext())
		active->SetException("Array index out of bounds");
}

static void* __cdecl indexArray(void*** arr, int idx)
{
	if (arr == 0 || *arr == 0 || idx < 0)
	{
		arrayIndexError();
		return 0;
	}
	return &(*arr)[idx];
}

static void* __cdecl indexDim1(BBArray* dim, int idx)
{
	if (dim == 0 || dim->data == 0 || idx < 0 || idx >= dim->scales[0])
	{
		arrayIndexError();
		return 0;
	}
	return static_cast<int*>(dim->data) + idx;
}

static void* __cdecl indexDim2(BBArray* dim, int idx1, int idx2)
{
	if (dim == 0 || dim->data == 0)
	{
		arrayIndexError();
		return 0;
	}
	int len1 = dim->scales[0];
	int len2 = len1 ? dim->scales[1] / len1 : 0;
	if (idx1 < 0 || idx1 >= len1 || idx2 < 0 || idx2 >= len2)
	{
		arrayIndexError();
		return 0;
	}
	return static_cast<int*>(dim->data) + idx2 * len1 + idx1;
}

static void* __cdecl indexDim3(BBArray* dim, int idx1, int idx2, int idx3)
{
	if (dim == 0 || dim->data == 0)
	{
		arrayIndexError();
		return 0;
	}
	int len1 = dim->scales[0];
	int len2 = len1 ? dim->scales[1] / len1 : 0;
	int len3 = dim->scales[1] ? dim->scales[2] / dim->scales[1] : 0;
	if (idx1 < 0 || idx1 >= len1 || idx2 < 0 || idx2 >= len2 || idx3 < 0 || idx3 >= len3)
	{
		arrayIndexError();
		return 0;
	}
	return static_cast<int*>(dim->data) + idx3 * dim->scales[1] + idx2 * dim->scales[0] + idx1;
}

static void __cdecl reinitDim1(BBArray* dim, int len1)
{
	if (dim == 0 || len1 < 0) { arrayIndexError(); return; }
	_bbUndimArray(dim);
	dim->scales[0] = len1;
	_bbDimArray(dim);
}

static void __cdecl reinitDim2(BBArray* dim, int len1, int len2)
{
	if (dim == 0 || len1 < 0 || len2 < 0) { arrayIndexError(); return; }
	_bbUndimArray(dim);
	dim->scales[0] = len1;
	dim->scales[1] = len2;
	_bbDimArray(dim);
}

static void __cdecl reinitDim3(BBArray* dim, int len1, int len2, int len3)
{
	if (dim == 0 || len1 < 0 || len2 < 0 || len3 < 0) { arrayIndexError(); return; }
	_bbUndimArray(dim);
	dim->scales[0] = len1;
	dim->scales[1] = len2;
	dim->scales[2] = len3;
	_bbDimArray(dim);
}

template <int I>
static int __cdecl lengthDim(BBArray* dim) {
	if constexpr (I == 0) return dim->scales[0];
	else return dim->scales[I] / dim->scales[I - 1];
}

static void registerDim(asIScriptEngine* engine, int dims)
{
	std::string name = "dim" + std::to_string(dims);

	engine->RegisterObjectType((name + "<class T>").c_str(), 0, asOBJ_REF | asOBJ_NOCOUNT | asOBJ_TEMPLATE);

	name += "<T>";

	if (dims > 0) engine->RegisterObjectMethod(name.c_str(), "int get_Length1() property", asFUNCTION(lengthDim<0>), asCALL_CDECL_OBJFIRST);
	if (dims > 1) engine->RegisterObjectMethod(name.c_str(), "int get_Length2() property", asFUNCTION(lengthDim<1>), asCALL_CDECL_OBJFIRST);
	if (dims > 2) engine->RegisterObjectMethod(name.c_str(), "int get_Length3() property", asFUNCTION(lengthDim<2>), asCALL_CDECL_OBJFIRST);

	asSFuncPtr ptr;
	switch (dims)
	{
	case 1: ptr = asFUNCTION(indexDim1); break;
	case 2: ptr = asFUNCTION(indexDim2); break;
	case 3: ptr = asFUNCTION(indexDim3); break;
	default: RTEX("RegisterObjectMethod: Too many parameters!");
	}

	std::string args = "int idx1";
	for (int i = 1; i < dims; ++i) {
		args += ", int idx" + std::to_string(i + 1);
	}

	engine->RegisterObjectMethod(name.c_str(), ("T& opIndex(" + args + ")").c_str(), ptr, asCALL_CDECL_OBJFIRST);

	switch (dims) {
	case 1: ptr = asFUNCTION(reinitDim1); break;
	case 2: ptr = asFUNCTION(reinitDim2); break;
	case 3: ptr = asFUNCTION(reinitDim3); break;
	default: RTEX("RegisterObjectMethod: Too many parameters!");
	}

	args = "int len1";
	for (int i = 1; i < dims; ++i) {
		args += ", int len" + std::to_string(i + 1);
	}

	engine->RegisterObjectMethod(name.c_str(), ("void Reinitialize(" + args + ")").c_str(), ptr, asCALL_CDECL_OBJFIRST);
}

std::string getAngelStackTrace()
{
	if (call_depth == 0) return "<no scripts in execution>";

	std::stringstream stackTrace;
	for (asUINT n = 0; n < ctx->GetCallstackSize(); n++)
	{
		const char* scriptSection;
		int column;
		asIScriptFunction* func = ctx->GetFunction(n);
		int line = ctx->GetLineNumber(n, &column, &scriptSection);
		stackTrace << scriptSection << ':' << func->GetDeclaration() << ":(" << line << ',' << column << ")\n";
	}
	return stackTrace.str();
}

void exceptionCallback(asIScriptContext* ctx, void*)
{
	appExceptionPending = true;
	try
	{
		throw;
	}
	catch (const std::exception& e)
	{
		ctx->SetException((std::string("Unknown Error: ") + e.what()).c_str());
	}
	catch (const bbEx& e)
	{
		ctx->SetException((std::string("Blitz3D Error: ") + e.err).c_str());
	}
	catch (...)
	{
	}
}

bool angel_create()
{
	engine = asCreateScriptEngine();
	if (!engine) { return false; }
	ctx = engine->CreateContext();
	if (!ctx) { return false; }
	if (engine->SetMessageCallback(asFUNCTION(messageCallback), nullptr, asCALL_STDCALL) < 0) { return false; }
	if (engine->SetTranslateAppExceptionCallback(asFUNCTION(exceptionCallback), nullptr, asCALL_STDCALL) < 0) { return false; }
	if (engine->SetEngineProperty(asEP_BUILD_WITHOUT_LINE_CUES, true) < 0) { return false; }
	if (engine->SetEngineProperty(asEP_ALLOW_IMPLICIT_HANDLE_TYPES, true) < 0) { return false; }

	if (engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, 1) < 0) { return false; }
	jit = new asCJITCompiler(0);
	if (engine->SetJITCompiler(jit) < 0) { return false; }

	RegisterScriptArray(engine, true);

	if (engine->RegisterObjectType("cstr", 0, asOBJ_REF | asOBJ_NOCOUNT | asOBJ_IMPLICIT_HANDLE) < 0) { return false; }

	if (engine->RegisterObjectType("string", sizeof(BBStr*), asOBJ_VALUE | asGetTypeTraits<BBStr*>()) < 0) { return false; }

	if (engine->RegisterObjectMethod("string", "cstr@ opImplCast()", asFUNCTION(strCStr), asCALL_CDECL_OBJFIRST) < 0) { return false; }

	if (engine->RegisterObjectBehaviour("string", asBEHAVE_CONSTRUCT, "void f()",
		asFUNCTION(constructString), asCALL_CDECL_OBJFIRST) < 0) { return false; }
	if (engine->RegisterObjectBehaviour("string", asBEHAVE_CONSTRUCT, "void f(cstr@)",
		asFUNCTION(constructStringCStr), asCALL_CDECL_OBJFIRST) < 0) { return false; }
	if (engine->RegisterObjectBehaviour("string", asBEHAVE_CONSTRUCT, "void f(const string& in)",
		asFUNCTION(copyConstructString), asCALL_CDECL_OBJFIRST) < 0) { return false; }
	if (engine->RegisterObjectBehaviour("string", asBEHAVE_DESTRUCT, "void f()",
		asFUNCTION(destructString), asCALL_CDECL_OBJFIRST) < 0) { return false; }
	if (engine->RegisterObjectMethod("string", "string& opAssign(const string& in)",
		asFUNCTION(assignString), asCALL_CDECL_OBJFIRST) < 0) { return false; }
	if (engine->RegisterObjectMethod("string", "string& opAddAssign(const string& in)",
		asFUNCTION(addAssignString), asCALL_CDECL_OBJFIRST) < 0) { return false; }

	if (engine->RegisterObjectMethod("string", "string opAdd(const string& in) const",
		asFUNCTION(addString), asCALL_CDECL_OBJFIRST) < 0) { return false; }

	if (engine->RegisterObjectMethod("string", "bool opEquals(const string& in) const",
		asFUNCTION(equalsString), asCALL_CDECL_OBJFIRST) < 0) { return false; }

	if (engine->RegisterObjectMethod("string", "uint get_Length() const property",
		asFUNCTION(lengthString), asCALL_CDECL_OBJFIRST))

	if (engine->RegisterObjectMethod("string", "string Substring(int start, int count=2147483647) const", asFUNCTION(substrString), asCALL_CDECL_OBJFIRST) < 0) { return false; }
	if (engine->RegisterObjectMethod("string", "int Find(const string&in toFind, int start=0) const", asFUNCTION(findString), asCALL_CDECL_OBJFIRST) < 0) { return false; }
	if (engine->RegisterObjectMethod("string", "string Replace(const string&in toReplace, const string&in replaceWith) const",
		asFUNCTION(replaceString), asCALL_CDECL_OBJFIRST) < 0) { return false; }

	if (engine->RegisterStringFactory("string", &stringFactory) < 0) { return false; }

	if (engine->RegisterObjectMethod("string", "float ParseFloat() const", asFUNCTION(stringToFloat<float>), asCALL_CDECL_OBJFIRST) < 0) { return false; }
	if (engine->RegisterObjectMethod("string", "float ParseDouble() const", asFUNCTION(stringToFloat<double>), asCALL_CDECL_OBJFIRST) < 0) { return false; }
	if (engine->RegisterObjectMethod("string", "int8 ParseInt8() const", asFUNCTION(stringToInt<std::int8_t>), asCALL_CDECL_OBJFIRST) < 0) { return false; }
	if (engine->RegisterObjectMethod("string", "uint8 ParseUInt8() const", asFUNCTION(stringToInt<std::uint8_t>), asCALL_CDECL_OBJFIRST) < 0) { return false; }
	if (engine->RegisterObjectMethod("string", "int16 ParseInt16() const", asFUNCTION(stringToInt<std::int16_t>), asCALL_CDECL_OBJFIRST) < 0) { return false; }
	if (engine->RegisterObjectMethod("string", "uint16 ParseUInt16() const", asFUNCTION(stringToInt<std::uint16_t>), asCALL_CDECL_OBJFIRST) < 0) { return false; }
	if (engine->RegisterObjectMethod("string", "int ParseInt() const", asFUNCTION(stringToInt<int>), asCALL_CDECL_OBJFIRST) < 0) { return false; }
	if (engine->RegisterObjectMethod("string", "uint ParseUInt() const", asFUNCTION(stringToInt<unsigned>), asCALL_CDECL_OBJFIRST) < 0) { return false; }
	if (engine->RegisterObjectMethod("string", "int64 ParseInt64() const", asFUNCTION(stringToInt<std::int64_t>), asCALL_CDECL_OBJFIRST) < 0) { return false; }
	if (engine->RegisterObjectMethod("string", "uint64 ParseUInt64() const", asFUNCTION(stringToInt<std::uint64_t>), asCALL_CDECL_OBJFIRST) < 0) { return false; }

	if (engine->RegisterGlobalFunction("string ToString(bool b)", asFUNCTION(boolToString), asCALL_CDECL) < 0) { return false; }
	if (engine->RegisterGlobalFunction("string ToString(float f)", asFUNCTION(toString<float>), asCALL_CDECL) < 0) { return false; }
	if (engine->RegisterGlobalFunction("string ToString(double d)", asFUNCTION(toString<double>), asCALL_CDECL) < 0) { return false; }
	if (engine->RegisterGlobalFunction("string ToString(int8 i)", asFUNCTION(toString<std::int8_t>), asCALL_CDECL) < 0) { return false; }
	if (engine->RegisterGlobalFunction("string ToString(uint8 u)", asFUNCTION(toString<std::uint8_t>), asCALL_CDECL) < 0) { return false; }
	if (engine->RegisterGlobalFunction("string ToString(int16 i)", asFUNCTION(toString<std::int16_t>), asCALL_CDECL) < 0) { return false; }
	if (engine->RegisterGlobalFunction("string ToString(uint16 u)", asFUNCTION(toString<std::uint16_t>), asCALL_CDECL) < 0) { return false; }
	if (engine->RegisterGlobalFunction("string ToString(int i)", asFUNCTION(toString<int>), asCALL_CDECL) < 0) { return false; }
	if (engine->RegisterGlobalFunction("string ToString(uint u)", asFUNCTION(toString<unsigned>), asCALL_CDECL) < 0) { return false; }
	if (engine->RegisterGlobalFunction("string ToString(int64 i)", asFUNCTION(toString<std::int64_t>), asCALL_CDECL) < 0) { return false; }
	if (engine->RegisterGlobalFunction("string ToString(uint64 u)", asFUNCTION(toString<std::uint64_t>), asCALL_CDECL) < 0) { return false; }

	registerStringArraySpecialization(engine);

	//RegisterScriptDictionary(engine);


	if (engine->RegisterObjectType("carray<class T>", 0, asOBJ_REF | asOBJ_NOCOUNT | asOBJ_TEMPLATE) < 0) { return false; }

	// TODO: This likely enables RCE without bounds checks, create wrapper type.
	if (engine->RegisterObjectMethod("carray<T>", "T& opIndex(int idx)",
		asFUNCTION(indexArray), asCALL_CDECL_OBJFIRST) < 0) { return false; }
	if (engine->RegisterObjectMethod("carray<T>", "const T& opIndex(int idx) const",
		asFUNCTION(indexArray), asCALL_CDECL_OBJFIRST) < 0) { return false; }

	for (int i = 1; i <= 3; ++i) {
		registerDim(engine, i);
	}

	// TODO:
	// - Bounds checks
	// - Iterable structs
	// - Fixed size arrays
	// - Custom type dims

	return true;
}

bool angel_destroy()
{
	factories.clear();
	bool ok = engine->ShutDownAndRelease() >= 0;
	delete jit;
	jit = nullptr;
	return ok;
}

void angel_link(void (*rtSym)(const char* sym, void* pc))
{
	rtSym("SetMessageCallback%callback", bbSetMessageCallback);
	rtSym("$GetDefaultNamespace", bbGetDefaultNamespace);
	rtSym("%SetDefaultNamespace$ns", bbSetDefaultNamespace);
	rtSym("%RegisterGlobalFunction$decl%ptr", bbRegisterGlobalFunction);
	rtSym("%RegisterObjectMethod$type$decl%ptr%isLast=0", bbRegisterObjectMethod);
	rtSym("%RegisterGlobalProperty$decl%ptr", bbRegisterGlobalProperty);
	rtSym("%RegisterTypeFromPtr$name%handle", bbRegisterTypeFromPtr);
	rtSym("%RegisterType$name", bbRegisterType);
	rtSym("%RegisterTypeInheritance$type$base", bbRegisterTypeInheritance);
	rtSym("%RegisterTypeConstructor$type$decl%ptr", bbRegisterTypeConstructor);
	rtSym("%RegisterTypeField$name$decl%index", bbRegisterTypeField);
	rtSym("%RegisterEnum$name", bbRegisterEnum);
	rtSym("%RegisterEnumValue$enumName$valueName%value", bbRegisterEnumValue);
	rtSym("%BeginModule$name", bbBeginModule);
	rtSym("%ModuleAddFile$file", bbModuleAddFile);
	rtSym("%EndModule", bbEndModule);
	rtSym("%GetModule$name", bbGetModule);
	rtSym("%GetModuleFunction%module$func", bbGetModuleFunction);
	rtSym("FreeModule%module", bbFreeModule);
	rtSym("$GetDeclarations%module=0", bbGetDeclarations);
	rtSym("PrepareFunction%argc", bbPrepareFunction);
	rtSym("SetArgInt%idx%arg", bbSetArgInt);
	rtSym("SetArgFloat%idx#arg", bbSetArgFloat);
	rtSym("SetArgString%idx$arg", bbSetArgString);
	rtSym("SetArgObj%idx%arg", bbSetArgObj);
	rtSym("SetArgIntObj%idx%arg", bbSetArgIntObj);
	rtSym("%ExecuteFunction%func%retPtr=0", bbExecuteFunction);
	rtSym("%ExecuteString$code", bbExecuteString);
}
