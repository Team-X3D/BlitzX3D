#include "std.h"
#include "bbstream.h"
#include "../MultiLang/MultiLang.h"

static std::unordered_set<bbStream*> stream_set;

void debugStream(bbStream* s, const char* function) {
	if (stream_set.count(s)) return;
	ErrorLog(function, MultiLang::stream_not_exist);
}

bbStream::bbStream() {
	stream_set.insert(this);
}

bbStream::~bbStream() {
	stream_set.erase(this);
}

int bbEof(bbStream* s) {
	debugStream(s, "Eof");
	return s->eof();
}

int bbReadAvail(bbStream* s) {
	debugStream(s, "ReadAvail");
	return s->avail();
}

int bbReadByte(bbStream* s) {
	debugStream(s, "ReadByte");
	try {
		int n = 0;
		s->read((char*)&n, 1);
		return n;
	}
	catch (std::exception& ex) {
		ErrorLog("ReadByte", MultiLang::readbyte_invalid_byte);
	}
}

int bbReadShort(bbStream* s) {
	debugStream(s, "ReadShort");
	try {
		int n = 0;
		s->read((char*)&n, 2);
		return n;
	}
	catch (std::exception& ex) {
		ErrorLog("ReadShort", MultiLang::readshort_invalid_short);
	}
}

int bbReadInt(bbStream* s) {
	debugStream(s, "ReadInt");
	try {
		int n = 0;
		s->read((char*)&n, 4);
		return n;
	}
	catch (std::exception& ex) {
		ErrorLog("ReadInt", MultiLang::readint_invalid_int);
	}
}

float bbReadFloat(bbStream* s) {
	debugStream(s, "ReadFloat");
	try {
		float n = 0;
		s->read((char*)&n, 4);
		return n;
	}
	catch (std::exception& ex) {
		ErrorLog("ReadFloat", MultiLang::readfloat_invalid_float);
	}
}

BBStr* bbReadString(bbStream* s) {
	debugStream(s, "ReadString");
	try {
		int len;
		BBStr* str = new BBStr();
		if (s->read((char*)&len, 4)) {
			char* buff = new char[len];
			if (s->read(buff, len)) {
				*str = std::string(buff, len);
			}
			delete[] buff;
		}
		return str;
	}
	catch (std::exception& ex) {
		ErrorLog("ReadString", MultiLang::readstring_invalid_string);
	}
}

BBStr* bbReadLine(bbStream* s) {
	debugStream(s, "ReadLine");
	unsigned char c;
	BBStr* str = new BBStr();
	for (;;) {
		if (s->read((char*)&c, 1) != 1) break;
		if (c == '\n') break;
		if (c != '\r') *str += c;
	}
	return str;
}

void bbWriteByte(bbStream* s, int n) {
	debugStream(s, "WriteByte");
	s->write((char*)&n, 1);
}

void bbWriteShort(bbStream* s, int n) {
	debugStream(s, "WriteShort");
	s->write((char*)&n, 2);
}

void bbWriteInt(bbStream* s, int n) {
	debugStream(s, "WriteInt");
	s->write((char*)&n, 4);
}

void bbWriteFloat(bbStream* s, float n) {
	debugStream(s, "WriteFloat");
	s->write((char*)&n, 4);
}

void bbWriteString(bbStream* s, BBStr* t) {
	debugStream(s, "WriteString");
	int n = t->size();
	s->write((char*)&n, 4);
	s->write(t->data(), t->size());
	delete t;
}

void bbWriteLine(bbStream* s, BBStr* t) {
	debugStream(s, "WriteLine");
	s->write(t->data(), t->size());
	s->write("\r\n", 2);
	delete t;
}

void bbCopyStream(bbStream* s, bbStream* d, int buff_size) {
	debugStream(s, "CopyStream");
	debugStream(d, "CopyStream");
	if (buff_size < 1 || buff_size>1024 * 1024) ErrorLog("CopyStream", MultiLang::illegal_buffer_size);
	char* buff = new char[buff_size];
	while (s->eof() == 0 && d->eof() == 0) {
		int n = s->read(buff, buff_size);
		d->write(buff, n);
		if (n < buff_size) break;
	}
	delete buff;
}

void bbFlushFile(bbStream* s) {
	debugStream(s, "FlushFile");
	s->flush();
}

bool stream_create() {
	return true;
}

bool stream_destroy() {
	return true;
}

void stream_link(void(*rtSym)(const char*, void*)) {
	rtSym("%Eof%stream", bbEof);
	rtSym("%ReadAvail%stream", bbReadAvail);
	rtSym("%ReadByte%stream", bbReadByte);
	rtSym("%ReadShort%stream", bbReadShort);
	rtSym("%ReadInt%stream", bbReadInt);
	rtSym("#ReadFloat%stream", bbReadFloat);
	rtSym("$ReadString%stream", bbReadString);
	rtSym("$ReadLine%stream", bbReadLine);
	rtSym("WriteByte%stream%byte", bbWriteByte);
	rtSym("WriteShort%stream%short", bbWriteShort);
	rtSym("WriteInt%stream%int", bbWriteInt);
	rtSym("WriteFloat%stream#float", bbWriteFloat);
	rtSym("WriteString%stream$string", bbWriteString);
	rtSym("WriteLine%stream$string", bbWriteLine);
	rtSym("CopyStream%src_stream%dest_stream%buffer_size=16384", bbCopyStream);
	rtSym("FlushFile%stream", bbFlushFile);
}