
#include <cassert>
#include <algorithm>
#include <memory>
#include <vector>
#include <scriptarray/scriptarray.h>

#include "basic.h"
#include "bbangel_string_array_specilization.h"

// TODO: Consider pooling?
struct string_vector : public  std::vector<std::unique_ptr<BBStr>>
{
	string_vector() { }
	string_vector(std::size_t count) : std::vector<std::unique_ptr<BBStr>>(count) { }

	string_vector& operator=(const string_vector& other)
	{
		if (this == &other)
			return *this;

		resize(other.size());
		for (asUINT i = 0; i < other.size(); ++i)
		{
			data()[i] = std::make_unique<BBStr>(*other[i]);
		}

		return *this;
	}

	void AddRef()
	{
		++ref_count;
	}

	void Release()
	{
		if (--ref_count == 0)
			delete this;
	}

	std::unique_ptr<BBStr>& At(asUINT idx) { return data()[idx]; }

	asUINT ForBegin() const
	{
		return 0;
	}

	asUINT ForEnd(asUINT iter) const
	{
		if (!this || iter >= size())
			return 1;
		else
			return 0;
	}

	asUINT ForNext(asUINT iter) const
	{
		return iter + 1;
	}

	asUINT ForValue1(asUINT iter) const
	{
		return iter;
	}

	void InsertAtValue(asUINT index, const BBStr*& value)
	{
		insert(begin() + index, std::make_unique<BBStr>(*value));
	}

	void InsertAtArray(asUINT index, const string_vector& arr)
	{
		for (auto& value : arr)
		{
			insert(begin() + index++, std::make_unique<BBStr>(*value));
		}
	}

	void InsertLast(const BBStr*& value)
	{
		push_back(std::make_unique<BBStr>(*value));
	}

	void RemoveAt(asUINT index)
	{
		erase(begin() + index);
	}

	void RemoveLast()
	{
		pop_back();
	}

	void RemoveRange(asUINT start, asUINT count)
	{
		erase(begin() + start, begin() + start + count);
	}

	void Reserve(asUINT length)
	{
		reserve(length);
	}

	void Resize(asUINT length)
	{
		asUINT old_length = size();
		resize(length);
		for (asUINT i = old_length; i < length; ++i)
		{
			data()[i] = std::make_unique<BBStr>();
		}
	}

	void SortAscAll()
	{
		SortAscRange(0, size());
	}

	void SortAscRange(asUINT startAt, asUINT count)
	{
		std::sort(begin() + startAt, begin() + startAt + count, [](const auto& lhs, const auto& rhs)
		{
			return lhs->compare(*rhs);
		});
	}

	void SortDescAll()
	{
		SortDescRange(0, size());
	}

	void SortDescRange(asUINT startAt, asUINT count)
	{
		std::sort(begin() + startAt, begin() + startAt + count, [](const auto& lhs, const auto& rhs)
		{
			return -lhs->compare(*rhs);
		});
	}

	void Reverse()
	{
		std::reverse(begin(), end());
	}

	int FindValue(const BBStr*& value) const
	{
		return FindFrom(0, value);
	}

	int FindFrom(asUINT startAt, const BBStr*& value) const
	{
		for (asUINT i = startAt; i < size(); ++i)
		{
			if (*data()[i] == *value)
				return static_cast<int>(i);
		}
		return -1;
	}

	bool Equals(const string_vector& other) const
	{
		if (size() != other.size())
			return false;

		for (asUINT i = 0; i < size(); ++i)
		{
			if (*data()[i] != *other[i])
				return false;
		}

		return true;
	}

	bool IsEmpty() const
	{
		return empty();
	}

	asUINT GetLength() const
	{
		return size();
	}

	void SetLength(asUINT length)
	{
		Resize(length);
	}

private:
	unsigned ref_count = 1;
};

string_vector* createEmpty()
{
	return new string_vector();
}

string_vector* createSized(unsigned length)
{
	auto ret = new string_vector();
	ret->Resize(length);
	return ret;
}

string_vector* createRepeat(unsigned length, const BBStr*& value)
{
	auto ret = new string_vector(length);
	for (auto& str : *ret)
		str = std::make_unique<BBStr>(*value);
	return ret;
}

string_vector* createFromList(void* listBuffer)
{
	auto count = *static_cast<asUINT*>(listBuffer);
	auto ret = new string_vector(count);
	for (asUINT i = 0; i < count; ++i)
		ret->data()[i] = std::make_unique<BBStr>(*static_cast<BBStr**>(listBuffer)[1 + i]);
	return ret;
}

void registerStringArraySpecialization(asIScriptEngine* engine)
{
	int r = 0;

	r = engine->RegisterObjectType("array<string>", 0, asOBJ_REF); assert(r >= 0);

	// Templates receive the object type as the first parameter. To the script writer this is hidden
	r = engine->RegisterObjectBehaviour("array<string>", asBEHAVE_FACTORY, "array<string>@ f()", asFUNCTION(createEmpty), asCALL_STDCALL); assert(r >= 0);
	r = engine->RegisterObjectBehaviour("array<string>", asBEHAVE_FACTORY, "array<string>@ f(uint length) explicit", asFUNCTION(createSized), asCALL_STDCALL); assert(r >= 0);
	r = engine->RegisterObjectBehaviour("array<string>", asBEHAVE_FACTORY, "array<string>@ f(uint length, const string &in value)", asFUNCTION(createRepeat), asCALL_STDCALL); assert(r >= 0);

	// Register the factory that will be used for initialization lists
	r = engine->RegisterObjectBehaviour("array<string>", asBEHAVE_LIST_FACTORY, "array<string>@ f(int&in list) {repeat string}", asFUNCTION(createFromList), asCALL_STDCALL); assert(r >= 0);

	// The memory management methods
	r = engine->RegisterObjectBehaviour("array<string>", asBEHAVE_ADDREF, "void f()", asMETHOD(string_vector, AddRef), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectBehaviour("array<string>", asBEHAVE_RELEASE, "void f()", asMETHOD(string_vector, Release), asCALL_THISCALL); assert(r >= 0);

	// The index operator returns the template subtype
	r = engine->RegisterObjectMethod("array<string>", "string &opIndex(uint index)", asMETHOD(string_vector, At), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "const string &opIndex(uint index) const", asMETHOD(string_vector, At), asCALL_THISCALL); assert(r >= 0);

	// Support for foreach
	r = engine->RegisterObjectMethod("array<string>", "uint opForBegin() const", asMETHOD(string_vector, ForBegin), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "bool opForEnd(uint) const", asMETHOD(string_vector, ForEnd), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "uint opForNext(uint) const", asMETHOD(string_vector, ForNext), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "const string &opForValue0(uint index) const", asMETHOD(string_vector, At), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "uint opForValue1(uint index) const", asMETHOD(string_vector, ForValue1), asCALL_THISCALL); assert(r >= 0);
	
	// The assignment operator
	r = engine->RegisterObjectMethod("array<string>", "array<string> &opAssign(const array<string>&in)", asMETHOD(string_vector, operator=), asCALL_THISCALL); assert(r >= 0);

	// Other methods
	r = engine->RegisterObjectMethod("array<string>", "void InsertAt(uint index, const string&in value)", asMETHOD(string_vector, InsertAtValue), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "void InsertAt(uint index, const array<string>&in arr)", asMETHOD(string_vector, InsertAtArray), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "void InsertLast(const string&in value)", asMETHOD(string_vector, InsertLast), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "void RemoveAt(uint index)", asMETHOD(string_vector, RemoveAt), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "void RemoveLast()", asMETHOD(string_vector, RemoveLast), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "void RemoveRange(uint start, uint count)", asMETHOD(string_vector, RemoveRange), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "void Reserve(uint length)", asMETHOD(string_vector, Reserve), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "void Resize(uint length)", asMETHOD(string_vector, Resize), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "void SortAsc()", asMETHOD(string_vector, SortAscAll), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "void SortAsc(uint startAt, uint count)", asMETHOD(string_vector, SortAscRange), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "void SortDesc()", asMETHOD(string_vector, SortDescAll), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "void SortDesc(uint startAt, uint count)", asMETHOD(string_vector, SortDescRange), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "void Reverse()", asMETHOD(string_vector, Reverse), asCALL_THISCALL); assert(r >= 0);
	// The token 'if_handle_then_const' is kept for signature compatibility.
	r = engine->RegisterObjectMethod("array<string>", "int Find(const string&in if_handle_then_const value) const", asMETHOD(string_vector, FindValue), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "int Find(uint startAt, const string&in if_handle_then_const value) const", asMETHOD(string_vector, FindFrom), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "int FindByRef(const string&in if_handle_then_const value) const", asMETHOD(string_vector, FindValue), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "int FindByRef(uint startAt, const string&in if_handle_then_const value) const", asMETHOD(string_vector, FindFrom), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "bool opEquals(const array<string>&in) const", asMETHOD(string_vector, Equals), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "bool get_IsEmpty() const property", asMETHOD(string_vector, IsEmpty), asCALL_THISCALL); assert(r >= 0);

	// Sort with callback for comparison
	//r = engine->RegisterFuncdef("bool array<string>::less(const string&in if_handle_then_const a, const string&in if_handle_then_const b)"); assert(r >= 0);
	//r = engine->RegisterObjectMethod("array<string>", "void Sort(const less &in, uint startAt = 0, uint count = uint(-1))", asMETHOD(string_vector, SortWithLambda), asCALL_THISCALL); assert(r >= 0);

	// Register virtual properties
	r = engine->RegisterObjectMethod("array<string>", "uint get_Length() const property", asMETHOD(string_vector, GetLength), asCALL_THISCALL); assert(r >= 0);
	r = engine->RegisterObjectMethod("array<string>", "void set_Length(uint) property", asMETHOD(string_vector, SetLength), asCALL_THISCALL); assert(r >= 0);
}
