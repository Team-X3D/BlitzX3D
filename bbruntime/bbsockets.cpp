#include "std.h"
#include "bbsockets.h"
#include <wininet.h>
#include <WinDNS.h>
#include "../MultiLang/MultiLang.h"
#include "../gxruntime/gxutf8.h"

#pragma comment (lib, "Urlmon.lib")
#pragma comment (lib, "Dnsapi.lib")
static bool socks_ok;
static WSADATA wsadata;
static int recv_timeout;
static int read_timeout;
static int accept_timeout;

static void close(SOCKET sock, int e) {
	if (e < 0) {
		int opt = 1;
		setsockopt(sock, SOL_SOCKET, SO_DONTLINGER, (char*)&opt, sizeof(opt));
	}
	closesocket(sock);
}

class UDPStream;
class TCPStream;
class TCPServer;

static std::unordered_set<UDPStream*> udp_set;
static std::unordered_set<TCPStream*> tcp_set;
static std::unordered_set<TCPServer*> server_set;

class UDPStream : public bbStream {
public:
	UDPStream(SOCKET s);
	~UDPStream();

	int read(char* buff, int size);
	int write(const char* buff, int size);
	int avail();
	int eof();

	int recv();
	int send(int ip, int port);
	int getIP();
	int getPort();
	int getMsgIP();
	int getMsgPort();

private:
	SOCKET sock;
	std::vector<char> in_buf, out_buf;
	sockaddr_in addr, in_addr, out_addr;
	int in_get, e;
};

UDPStream::UDPStream(SOCKET s) :sock(s), in_get(0), e(0) {
	int len = sizeof(addr);
	getsockname(s, (sockaddr*)&addr, &len);
	in_addr = out_addr = addr;
}

UDPStream::~UDPStream() {
	close(sock, e);
}

int UDPStream::read(char* buff, int size) {
	if (e) return 0;
	int n = (int)in_buf.size() - in_get;
	if (n <= 0 || size <= 0) return 0;
	if (size > n) size = n;
	memcpy(buff, &in_buf[in_get], size);
	in_get += size;
	return size;
}

int UDPStream::write(const char* buff, int size) {
	if (e) return 0;
	out_buf.insert(out_buf.end(), buff, buff + size);
	return size;
}

int UDPStream::avail() {
	if (e) return 0;
	return in_buf.size() - in_get;
}

int UDPStream::eof() {
	return e ? e : in_get == in_buf.size();
}

//fill buffer, return sender
int UDPStream::recv() {
	if (e) return 0;
	int tout;
	if (recv_timeout) tout = gx_runtime->getMilliSecs() + recv_timeout;
	for (;;) {
		int dt = 0;
		if (recv_timeout) {
			dt = tout - gx_runtime->getMilliSecs();
			if (dt < 0) dt = 0;
		}
		fd_set fd = { 1,sock };
		timeval tv = { dt / 1000,(dt % 1000) * 1000 };
		int n = ::select(0, &fd, 0, 0, &tv);
		if (!n) return 0;
		if (n != 1) { e = -1; return 0; }
		unsigned long sz = -1;
		if (ioctlsocket(sock, FIONREAD, &sz)) { e = -1; return 0; }
		in_buf.resize(sz); in_get = 0;
		int len = sizeof(in_addr);
		n = ::recvfrom(sock, (char*)in_buf.data(), sz, 0, (sockaddr*)&in_addr, &len);
		if (n == SOCKET_ERROR) continue;	//{ e=-1;return 0; }
		in_buf.resize(n);
		return getMsgIP();
	}
	return 0;
}

//send, empty buffer
int UDPStream::send(int ip, int port) {
	if (e) return 0;
	int sz = out_buf.size();
	out_addr.sin_addr.S_un.S_addr = htonl(ip);
	out_addr.sin_port = htons(port ? port : addr.sin_port);
	int n = ::sendto(sock, (char*)out_buf.data(), sz, 0, (sockaddr*)&out_addr, sizeof(out_addr));
	if (n != sz) return e = -1;
	out_buf.clear();
	return sz;
}

int UDPStream::getIP() {
	return ntohl(addr.sin_addr.S_un.S_addr);
}

int UDPStream::getPort() {
	return ntohs(addr.sin_port);
}

int UDPStream::getMsgIP() {
	return ntohl(in_addr.sin_addr.S_un.S_addr);
}

int UDPStream::getMsgPort() {
	return ntohs(in_addr.sin_port);
}

class TCPStream : public bbStream {
public:
	TCPStream(SOCKET s, TCPServer* t);
	~TCPStream();

	int read(char* buff, int size);
	int write(const char* buff, int size);
	int avail();
	int eof();

	int getIP();
	int getPort();

private:
	SOCKET sock;
	TCPServer* server;
	int e, ip, port;
};

class TCPServer {
public:
	TCPServer(SOCKET S);
	~TCPServer();

	TCPStream* accept();

	void remove(TCPStream* s);

private:
	int e;
	SOCKET sock;
	std::set<TCPStream*> accepted_set;
};

TCPStream::TCPStream(SOCKET s, TCPServer* t) :sock(s), server(t), e(0) {
	sockaddr_in addr;
	int len = sizeof(addr);
	if (getpeername(s, (sockaddr*)&addr, &len)) {
		ip = port = 0;
		return;
	}
	ip = ntohl(addr.sin_addr.S_un.S_addr);
	port = ntohs(addr.sin_port);
}

TCPStream::~TCPStream() {
	if (server) server->remove(this);
	close(sock, e);
}

int TCPStream::read(char* buff, int size) {
	if (e) return 0;
	char* b = buff, * l = buff + size;
	int tout;
	if (read_timeout) tout = gx_runtime->getMilliSecs() + read_timeout;
	while (b < l) {
		int dt = 0;
		if (read_timeout) {
			dt = tout - gx_runtime->getMilliSecs();
			if (dt < 0) dt = 0;
		}
		fd_set fd = { 1,sock };
		timeval tv = { dt / 1000,(dt % 1000) * 1000 };
		int n = ::select(0, &fd, 0, 0, &tv);
		if (n != 1) { e = -1; break; }
		n = ::recv(sock, b, l - b, 0);
		if (n == 0) { e = 1; break; }
		if (n == SOCKET_ERROR) { e = -1; break; }
		b += n;
	}
	return b - buff;
}

int TCPStream::write(const char* buff, int size) {
	if (e) return 0;
	int n = ::send(sock, buff, size, 0);
	if (n == SOCKET_ERROR) { e = -1; return 0; }
	return n;
}

int TCPStream::avail() {
	unsigned long t;
	int n = ::ioctlsocket(sock, FIONREAD, &t);
	if (n == SOCKET_ERROR) { e = -1; return 0; }
	return t;
}

int TCPStream::eof() {
	if (e) return e;
	fd_set fd = { 1,sock };
	timeval tv = { 0,0 };
	switch (::select(0, &fd, 0, 0, &tv)) {
	case 0:break;
	case 1:if (!avail()) e = 1; break;
	default:e = -1;
	}
	return e;
}

int TCPStream::getIP() {
	return ip;
}

int TCPStream::getPort() {
	return port;
}

TCPServer::TCPServer(SOCKET s) :sock(s), e(0) {
}

TCPServer::~TCPServer() {
	while (accepted_set.size()) delete* accepted_set.begin();
	close(sock, e);
}

TCPStream* TCPServer::accept() {
	if (e) return 0;
	fd_set fd = { 1,sock };
	timeval tv = { accept_timeout / 1000,(accept_timeout % 1000) * 1000 };
	int n = ::select(0, &fd, 0, 0, &tv);
	if (n == 0) return 0;
	if (n != 1) { e = -1; return 0; }
	SOCKET t = ::accept(sock, 0, 0);
	if (t == INVALID_SOCKET) { e = -1; return 0; }
	TCPStream* s = new TCPStream(t, this);
	accepted_set.insert(s);
	return s;
}

void TCPServer::remove(TCPStream* s) {
	accepted_set.erase(s);
}

static inline void debugUDPStream(UDPStream* p, const char* function) {
	if (!udp_set.count(p)) {
		ErrorLog(function, MultiLang::udp_stream_not_exist);
	}
}

static inline void debugTCPStream(TCPStream* p, const char* function) {
	if (!tcp_set.count(p)) {
		ErrorLog(function, MultiLang::tcp_stream_not_exist);
	}
}

static inline void debugTCPServer(TCPServer* p, const char* function) {
	if (!server_set.count(p)) {
		ErrorLog(function, MultiLang::tcp_server_not_exist);
	}
}

static std::vector<int> host_ips;

int bbCountHostIPs(BBStr* host) {
	host_ips.clear();
	HOSTENT* h = gethostbyname(host->c_str());
	delete host; if (!h) return 0;
	char** p = h->h_addr_list;
	while (char* t = *p++) host_ips.push_back(ntohl(*(int*)t));
	return host_ips.size();
}

int bbHostIP(int index) {
	if (index<1 || index>host_ips.size()) {
		ErrorLog("HostIP", MultiLang::host_out_of_range);
	}
	return host_ips[index - 1];
}

UDPStream* bbCreateUDPStream(BBStr* ip, int port) {
	if (!socks_ok) return 0;
	SOCKET s = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (s != INVALID_SOCKET) {
		sockaddr_in addr = {AF_INET, htons(port)};
		addr.sin_addr.s_addr = ::inet_addr(ip->c_str()); delete ip;

		if (!::bind(s, (sockaddr*)&addr, sizeof(addr))) {
			UDPStream* p = new UDPStream(s);
			udp_set.insert(p);
			return p;
		}
		::closesocket(s);
	}
	return 0;
}

void bbCloseUDPStream(UDPStream* p) {
	debugUDPStream(p, "CloseUDPStream");
	udp_set.erase(p);
	delete p;
}

int bbRecvUDPMsg(UDPStream* p) {
	debugUDPStream(p, "RecvUDPMsg");
	return p->recv();
}

void bbSendUDPMsg(UDPStream* p, int ip, int port) {
	debugUDPStream(p, "SendUDPMsg");
	p->send(ip, port);
}

int bbUDPStreamIP(UDPStream* p) {
	debugUDPStream(p, "UDPStreamIP");
	return p->getIP();
}

int bbUDPStreamPort(UDPStream* p) {
	debugUDPStream(p, "UDPStreamPort");
	return p->getPort();
}

int bbUDPMsgIP(UDPStream* p) {
	debugUDPStream(p, "UDPMsgIP");
	return p->getMsgIP();
}

int bbUDPMsgPort(UDPStream* p) {
	debugUDPStream(p, "UDPMsgPort");
	return p->getMsgPort();
}

void bbUDPTimeouts(int rt) {
	recv_timeout = rt;
}

BBStr* bbDottedIP(int ip) {
	return new BBStr(
		itoa((ip >> 24) & 255) + "." + itoa((ip >> 16) & 255) + "." +
		itoa((ip >> 8) & 255) + "." + itoa(ip & 255));
}

static int findHostIP(const std::string& t) {
	int ip = inet_addr(t.c_str());
	if (ip != INADDR_NONE) return ip;
	HOSTENT* h = gethostbyname(t.c_str());
	if (!h) return -1;
	char* p;
	for (char** list = h->h_addr_list; p = *list; ++list) {
		return *(int*)p;
	}
	return 0;
}

TCPStream* bbOpenTCPStream(BBStr* server, int port, int local_port) {
	if (!socks_ok) {
		delete server;
		return 0;
	}
	int ip = findHostIP(*server); delete server;
	if (ip == -1) return 0;
	SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
	if (s != INVALID_SOCKET) {
		if (local_port) {
			sockaddr_in addr = { AF_INET,htons(local_port) };
			if (::bind(s, (sockaddr*)&addr, sizeof(addr))) {
				::closesocket(s);
				return 0;
			}
		}
		sockaddr_in addr = { AF_INET,htons(port) };
		addr.sin_addr.S_un.S_addr = ip;
		if (!::connect(s, (sockaddr*)&addr, sizeof(addr))) {
			TCPStream* p = new TCPStream(s, 0);
			tcp_set.insert(p);
			return p;
		}
		::closesocket(s);
	}
	return 0;
}

void bbCloseTCPStream(TCPStream* p) {
	debugTCPStream(p, "CloseTCPStream");
	tcp_set.erase(p);
	delete p;
}

TCPServer* bbCreateTCPServer(int port) {
	SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
	if (s != INVALID_SOCKET) {
		sockaddr_in addr = { AF_INET,htons(port) };
		if (!::bind(s, (sockaddr*)&addr, sizeof(addr))) {
			if (!::listen(s, SOMAXCONN)) {
				TCPServer* p = new TCPServer(s);
				server_set.insert(p);
				return p;
			}
		}
		::closesocket(s);
	}
	return 0;
}

void  bbCloseTCPServer(TCPServer* p) {
	debugTCPServer(p, "CloseTCPServer");
	server_set.erase(p);
	delete p;
}

TCPStream* bbAcceptTCPStream(TCPServer* server) {
	debugTCPServer(server, "AcceptTCPStream");
	if (!gx_runtime->idle()) RTEX(0);
	if (TCPStream* tcp = server->accept()) {
		tcp_set.insert(tcp);
		return tcp;
	}
	return 0;
}

int bbTCPStreamIP(TCPStream* p) {
	debugTCPStream(p, "TCPStreamIP");
	return p->getIP();
}

int bbTCPStreamPort(TCPStream* p) {
	debugTCPStream(p, "TCPStreamPort");
	return p->getPort();
}

void bbTCPTimeouts(int rt, int at) {
	read_timeout = rt;
	accept_timeout = at;
}

BBStr* bbParseDomainTXT(BBStr* txt, BBStr* name) {
	std::string s1 = txt->c_str();
	std::string s2 = name->c_str();
	std::string result = "";
	int n, a;
	if ((n = s1.find(s2 + "=")) != std::string::npos)
		result = s1.substr(n);
	if ((a = result.find(';')) != std::string::npos)
		result = result.substr(s2.length() + 1, a - s2.length() - 1);
	delete txt; delete name;
	return new BBStr(result);
}

BBStr* bbGetDomainTXT(BBStr* domain) {
	PDNS_RECORD pResult = NULL;
	DnsQuery_A(domain->c_str(), DNS_TYPE_TEXT, DNS_QUERY_BYPASS_CACHE, NULL, &pResult, NULL);
	if (!pResult) {
		delete domain;
		return new BBStr("");
	}
	std::string record = pResult->Data.TXT.pStringArray[0];
	delete domain;
	DnsRecordListFree(pResult, DnsFreeRecordListDeep);
	return new BBStr(record);
}

bool sockets_create() {
	socks_ok = WSAStartup(0x0101, &wsadata) == 0;
	recv_timeout = 0;
	read_timeout = 10000;
	accept_timeout = 0;
	return true;
}

bool sockets_destroy() {
	while (udp_set.size()) bbCloseUDPStream(*udp_set.begin());
	while (tcp_set.size()) bbCloseTCPStream(*tcp_set.begin());
	while (server_set.size()) bbCloseTCPServer(*server_set.begin());
	if (socks_ok) WSACleanup();
	return true;
}

int bbDownloadFile(BBStr* url, BBStr* file) {
	HRESULT result = URLDownloadToFileA(NULL, url->c_str(), file->c_str(), NULL, NULL);
	delete url; delete file;
	return result;
}

int bbHttpStatus(BBStr* url) {
	HINTERNET hInternet = InternetOpenA("Blitz3D", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
	if (!hInternet) { delete url; return 0; }

	HINTERNET hUrl = InternetOpenUrlA(hInternet, url->c_str(), NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
	delete url;
	if (!hUrl) { InternetCloseHandle(hInternet); return 0; }

	DWORD status = 0, len = sizeof(status);
	if (!HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, NULL))
		status = 0;

	InternetCloseHandle(hUrl);
	InternetCloseHandle(hInternet);
	return (int)status;
}

void sockets_link(void(*rtSym)(const char*, void*)) {
	rtSym("$DottedIP%IP", bbDottedIP);
	rtSym("%CountHostIPs$host_name", bbCountHostIPs);
	rtSym("%HostIP%host_index", bbHostIP);

	rtSym("%CreateUDPStream$ip%port=0", bbCreateUDPStream);
	rtSym("CloseUDPStream%udp_stream", bbCloseUDPStream);
	rtSym("SendUDPMsg%udp_stream%dest_ip%dest_port=0", bbSendUDPMsg);
	rtSym("%RecvUDPMsg%udp_stream", bbRecvUDPMsg);
	rtSym("%UDPStreamIP%udp_stream", bbUDPStreamIP);
	rtSym("%UDPStreamPort%udp_stream", bbUDPStreamPort);
	rtSym("%UDPMsgIP%udp_stream", bbUDPMsgIP);
	rtSym("%UDPMsgPort%udp_stream", bbUDPMsgPort);
	rtSym("UDPTimeouts%recv_timeout", bbUDPTimeouts);

	rtSym("%OpenTCPStream$server%server_port%local_port=0", bbOpenTCPStream);
	rtSym("CloseTCPStream%tcp_stream", bbCloseTCPStream);
	rtSym("%CreateTCPServer%port", bbCreateTCPServer);
	rtSym("CloseTCPServer%tcp_server", bbCloseTCPServer);
	rtSym("%AcceptTCPStream%tcp_server", bbAcceptTCPStream);
	rtSym("%TCPStreamIP%tcp_stream", bbTCPStreamIP);
	rtSym("%TCPStreamPort%tcp_stream", bbTCPStreamPort);
	rtSym("TCPTimeouts%read_millis%accept_millis", bbTCPTimeouts);

	rtSym("$GetDomainTXT$domain", bbGetDomainTXT);
	rtSym("$ParseDomainTXT$txt$name", bbParseDomainTXT);

	rtSym("%DownloadFile$url$file", bbDownloadFile);
	rtSym("%HttpStatus$url", bbHttpStatus);
}