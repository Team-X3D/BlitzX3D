#include "std.h"
#include "bbnet_transport.h"

#include <winsock2.h>
#include <algorithm>

static const int DIRECT_MAX_FRAME = 16 * 1024 * 1024;

static void appendFrame(std::vector<char>& out, const void* data, int size) {
	unsigned n = (unsigned)size;
	out.push_back((char)(n & 0xff));
	out.push_back((char)((n >> 8) & 0xff));
	out.push_back((char)((n >> 16) & 0xff));
	out.push_back((char)((n >> 24) & 0xff));
	out.insert(out.end(), (const char*)data, (const char*)data + size);
}

class DirectTransport : public INetTransport {
public:
	DirectTransport() : listener_(INVALID_SOCKET), nextId_(1), port_(0) {}
	~DirectTransport() { close(); }

	const char* name() const { return "direct"; }

	bool listen(int port) {
		listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
		if (listener_ == INVALID_SOCKET) return false;
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = INADDR_ANY;
		a.sin_port = htons((u_short)port);
		if (::bind(listener_, (sockaddr*)&a, sizeof(a)) || ::listen(listener_, SOMAXCONN)) {
			::closesocket(listener_);
			listener_ = INVALID_SOCKET;
			return false;
		}
		setNonBlocking(listener_);
		port_ = port;
		return true;
	}

	bool connect(const char* host, int port) {
		SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
		if (s == INVALID_SOCKET) return false;
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_port = htons((u_short)port);
		a.sin_addr.s_addr = ::inet_addr(host);
		if (a.sin_addr.s_addr == INADDR_NONE) {
			HOSTENT* h = ::gethostbyname(host);
			if (!h || !h->h_addr_list[0]) { ::closesocket(s); return false; }
			memcpy(&a.sin_addr, h->h_addr_list[0], 4);
		}
		if (::connect(s, (sockaddr*)&a, sizeof(a))) { ::closesocket(s); return false; }
		setNonBlocking(s);
		NetPeerId id = addPeer(s);
		NetEvent ev = {};
		ev.type = NetEvent::Connect;
		ev.peer = id;
		events_.push_back(ev);
		port_ = port;
		return true;
	}

	void poll() {
		if (listener_ != INVALID_SOCKET) {
			for (;;) {
				SOCKET s = ::accept(listener_, 0, 0);
				if (s == INVALID_SOCKET) break;
				setNonBlocking(s);
				NetPeerId id = addPeer(s);
				NetEvent ev = {};
				ev.type = NetEvent::Connect;
				ev.peer = id;
				events_.push_back(ev);
			}
		}
		for (auto it = peers_.begin(); it != peers_.end();) {
			SOCKET s = it->second.sock;
			bool dead = false;
			fd_set fd;
			FD_ZERO(&fd);
			FD_SET(s, &fd);
			timeval tv = { 0, 0 };
			int r = ::select(0, &fd, 0, 0, &tv);
			if (r > 0 && FD_ISSET(s, &fd)) {
				char buf[4096];
				int n = ::recv(s, buf, sizeof(buf), 0);
				if (n > 0) {
					it->second.in.insert(it->second.in.end(), buf, buf + n);
					if (!parse(it->first, it->second)) dead = true;
				} else if (n == 0) {
					dead = true;
				} else if (::WSAGetLastError() != WSAEWOULDBLOCK) {
					dead = true;
				}
			}
			if (dead) {
				NetEvent ev = {};
				ev.type = NetEvent::Disconnect;
				ev.peer = it->first;
				events_.push_back(ev);
				::closesocket(s);
				it = peers_.erase(it);
			} else {
				++it;
			}
		}
	}

	void send(NetPeerId to, const void* data, int size, NetReliability reliability) {
		auto it = peers_.find(to);
		if (it == peers_.end()) return;
		std::vector<char> f;
		appendFrame(f, data, size);
		SOCKET s = it->second.sock;
		const char* p = f.data();
		int rem = (int)f.size();
		while (rem > 0) {
			fd_set wf;
			FD_ZERO(&wf);
			FD_SET(s, &wf);
			timeval tv = { 5, 0 };
			if (::select(0, 0, &wf, 0, &tv) <= 0) break;
			int n = ::send(s, p, rem, 0);
			if (n == SOCKET_ERROR) {
				if (::WSAGetLastError() == WSAEWOULDBLOCK) continue;
				break;
			}
			p += n;
			rem -= n;
		}
	}

	void broadcast(const void* data, int size, NetReliability reliability) {
		for (auto& kv : peers_) send(kv.first, data, size, reliability);
	}

	void closePeer(NetPeerId id) {
		auto it = peers_.find(id);
		if (it == peers_.end()) return;
		::closesocket(it->second.sock);
		peers_.erase(it);
	}

	void close() {
		for (auto& kv : peers_) ::closesocket(kv.second.sock);
		peers_.clear();
		if (listener_ != INVALID_SOCKET) {
			::closesocket(listener_);
			listener_ = INVALID_SOCKET;
		}
	}

	std::vector<NetEvent> drain() {
		std::vector<NetEvent> out;
		out.swap(events_);
		return out;
	}

	int hostPort() const { return port_; }

private:
	struct Peer {
		SOCKET sock;
		std::vector<char> in;
	};

	void setNonBlocking(SOCKET s) {
		u_long nb = 1;
		::ioctlsocket(s, FIONBIO, &nb);
	}

	NetPeerId addPeer(SOCKET s) {
		NetPeerId id = nextId_++;
		Peer p;
		p.sock = s;
		peers_[id] = p;
		return id;
	}

	bool parse(NetPeerId id, Peer& p) {
		for (;;) {
			if (p.in.size() < 4) return true;
			unsigned n = (unsigned char)p.in[0] |
				((unsigned char)p.in[1] << 8) |
				((unsigned char)p.in[2] << 16) |
				((unsigned char)p.in[3] << 24);
			if (n > (unsigned)DIRECT_MAX_FRAME) return false;
			if (p.in.size() < 4 + n) return true;
			NetEvent ev = {};
			ev.type = NetEvent::Receive;
			ev.peer = id;
			ev.data.assign(p.in.begin() + 4, p.in.begin() + 4 + n);
			events_.push_back(ev);
			p.in.erase(p.in.begin(), p.in.begin() + 4 + n);
		}
	}

	SOCKET listener_;
	std::map<NetPeerId, Peer> peers_;
	std::vector<NetEvent> events_;
	NetPeerId nextId_;
	int port_;
};

INetTransport* createDirectTransport() {
	return new DirectTransport();
}
