#include "std.h"
#include "bbnet_transport.h"

#include <winsock2.h>
#include <cstring>
#include <atomic>
#include <memory>
#include <thread>

static const int DIRECT_MAX_FRAME = 16 * 1024 * 1024;
static const int DIRECT_MAX_UDP = 1200;
static const unsigned long long DIRECT_CONNECT_TIMEOUT_MS = 5000;
static const unsigned char FRAME_TOKEN = 0;
static const unsigned char FRAME_USER = 1;

static void appendToken(std::vector<char>& b, unsigned long long token) {
	for (int i = 0; i < 8; ++i) b.push_back((char)((token >> (8 * i)) & 0xff));
}

static unsigned long long readToken(const char* p) {
	unsigned long long t = 0;
	for (int i = 0; i < 8; ++i) t |= ((unsigned long long)(unsigned char)p[i]) << (8 * i);
	return t;
}

static void appendFrame(std::vector<char>& out, unsigned char kind, const void* data, int size) {
	unsigned n = (unsigned)size;
	out.push_back((char)(n & 0xff));
	out.push_back((char)((n >> 8) & 0xff));
	out.push_back((char)((n >> 16) & 0xff));
	out.push_back((char)((n >> 24) & 0xff));
	out.push_back((char)kind);
	out.insert(out.end(), (const char*)data, (const char*)data + size);
}

class DirectTransport : public INetTransport {
public:
	DirectTransport()
		: listener_(INVALID_SOCKET), udp_(INVALID_SOCKET), isServer_(false),
		nextId_(1), nextToken_(1), port_(0) {
	}
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
		isServer_ = true;
		openUdp(port);
		port_ = port;
		return true;
	}

	bool connect(const char* host, int port) {
		if (connectSock_ != INVALID_SOCKET) {
			::closesocket(connectSock_);
			connectSock_ = INVALID_SOCKET;
		}
		resolve_.reset();
		SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
		if (s == INVALID_SOCKET) return false;
		setNonBlocking(s);
		connectSock_ = s;
		connectStartMs_ = GetTickCount64();
		connectAddr_ = {};
		connectAddr_.sin_family = AF_INET;
		connectAddr_.sin_port = htons((u_short)port);
		unsigned long ip = ::inet_addr(host);
		if (ip != INADDR_NONE) {
			connectAddr_.sin_addr.s_addr = ip;
			beginConnect();
		} else {
			auto res = std::make_shared<Resolve>();
			res->port = (u_short)port;
			resolve_ = res;
			std::string h = host;
			std::thread([res, h]() {
				HOSTENT* he = ::gethostbyname(h.c_str());
				if (he && he->h_addr_list[0]) {
					res->addr.sin_family = AF_INET;
					res->addr.sin_port = htons(res->port);
					memcpy(&res->addr.sin_addr, he->h_addr_list[0], 4);
					res->state.store(1);
				} else {
					res->state.store(2);
				}
			}).detach();
		}
		openUdp(0);
		port_ = port;
		return true;
	}

	void poll() {
		pollConnect();
		if (listener_ != INVALID_SOCKET) {
			for (;;) {
				SOCKET s = ::accept(listener_, 0, 0);
				if (s == INVALID_SOCKET) break;
				if (maxPeers_ > 0 && (int)peers_.size() >= maxPeers_) {
					::closesocket(s);
					continue;
				}
				setNonBlocking(s);
				NetPeerId id = addPeer(s);
				Peer& p = peers_[id];
				p.token = nextToken_++;
				tokenToPeer_[p.token] = id;
				char tb[8];
				for (int i = 0; i < 8; ++i) tb[i] = (char)((p.token >> (8 * i)) & 0xff);
				sendTcp(p, FRAME_TOKEN, tb, 8);
				NetEvent ev = {};
				ev.type = NetEvent::Connect;
				ev.peer = id;
				events_.push_back(ev);
			}
		}
		for (auto it = peers_.begin(); it != peers_.end();) {
			SOCKET s = it->second.sock;
			bool dead = it->second.dead;
			if (!dead) {
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
			}
			if (dead) {
				NetEvent ev = {};
				ev.type = NetEvent::Disconnect;
				ev.peer = it->first;
				events_.push_back(ev);
				if (s != INVALID_SOCKET) ::closesocket(s);
				if (it->second.token) tokenToPeer_.erase(it->second.token);
				it = peers_.erase(it);
			} else {
				++it;
			}
		}
		pollUdp();
	}

	void send(NetPeerId to, const void* data, int size, NetReliability reliability) {
		auto it = peers_.find(to);
		if (it == peers_.end()) return;
		Peer& p = it->second;
		if (p.dead) return;
		if (reliability == NetReliability::Unreliable && p.hasUdp && p.token && size <= DIRECT_MAX_UDP) {
			sendUdp(p, data, size);
		} else {
			sendTcp(p, FRAME_USER, data, size);
		}
	}

	void broadcast(const void* data, int size, NetReliability reliability) {
		for (auto& kv : peers_) send(kv.first, data, size, reliability);
	}

	void closePeer(NetPeerId id) {
		auto it = peers_.find(id);
		if (it == peers_.end()) return;
		if (it->second.sock != INVALID_SOCKET) ::closesocket(it->second.sock);
		if (it->second.token) tokenToPeer_.erase(it->second.token);
		peers_.erase(it);
	}

	void setMaxPeers(int max) {
		maxPeers_ = max > 0 ? max : 0;
	}

	void close() {
		if (connectSock_ != INVALID_SOCKET) {
			::closesocket(connectSock_);
			connectSock_ = INVALID_SOCKET;
		}
		resolve_.reset();
		for (auto& kv : peers_) {
			if (kv.second.sock != INVALID_SOCKET) ::closesocket(kv.second.sock);
		}
		peers_.clear();
		tokenToPeer_.clear();
		if (listener_ != INVALID_SOCKET) {
			::closesocket(listener_);
			listener_ = INVALID_SOCKET;
		}
		if (udp_ != INVALID_SOCKET) {
			::closesocket(udp_);
			udp_ = INVALID_SOCKET;
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
		SOCKET sock = INVALID_SOCKET;
		sockaddr_in udpAddr = {};
		bool hasUdp = false;
		bool dead = false;
		unsigned long long token = 0;
		std::vector<char> in;
	};

	struct Resolve {
		std::atomic<int> state{ 0 }; // 0 pending, 1 resolved, 2 failed
		sockaddr_in addr = {};
		u_short port = 0;
	};

	void beginConnect() {
		connecting_ = true;
		if (::connect(connectSock_, (sockaddr*)&connectAddr_, sizeof(connectAddr_)) == 0) {
			finishConnect();
		} else if (::WSAGetLastError() != WSAEWOULDBLOCK) {
			failConnect();
		}
	}

	void finishConnect() {
		connecting_ = false;
		NetPeerId id = addPeer(connectSock_);
		Peer& p = peers_[id];
		p.udpAddr = connectAddr_;
		p.hasUdp = true;
		connectSock_ = INVALID_SOCKET;
		NetEvent ev = {};
		ev.type = NetEvent::Connect;
		ev.peer = id;
		events_.push_back(ev);
	}

	void failConnect() {
		connecting_ = false;
		resolve_.reset();
		if (connectSock_ != INVALID_SOCKET) {
			::closesocket(connectSock_);
			connectSock_ = INVALID_SOCKET;
		}
		NetEvent ev = {};
		ev.type = NetEvent::Disconnect;
		ev.peer = 0;
		events_.push_back(ev);
	}

	void pollConnect() {
		if (connectSock_ == INVALID_SOCKET) return;
		unsigned long long now = GetTickCount64();
		if (resolve_) {
			int st = resolve_->state.load();
			if (st == 1) {
				connectAddr_ = resolve_->addr;
				resolve_.reset();
				beginConnect();
			} else if (st == 2 || now - connectStartMs_ > DIRECT_CONNECT_TIMEOUT_MS) {
				failConnect();
			}
			return;
		}
		if (!connecting_) return;
		fd_set wf;
		FD_ZERO(&wf);
		FD_SET(connectSock_, &wf);
		timeval tv = { 0, 0 };
		int r = ::select(0, 0, &wf, 0, &tv);
		if (r > 0 && FD_ISSET(connectSock_, &wf)) {
			int err = 0;
			int len = sizeof(err);
			::getsockopt(connectSock_, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
			if (err) failConnect();
			else finishConnect();
		} else if (now - connectStartMs_ > DIRECT_CONNECT_TIMEOUT_MS) {
			failConnect();
		}
	}

	void setNonBlocking(SOCKET s) {
		u_long nb = 1;
		::ioctlsocket(s, FIONBIO, &nb);
	}

	void openUdp(int port) {
		udp_ = ::socket(AF_INET, SOCK_DGRAM, 0);
		if (udp_ == INVALID_SOCKET) return;
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = INADDR_ANY;
		a.sin_port = htons((u_short)port);
		if (::bind(udp_, (sockaddr*)&a, sizeof(a))) {
			::closesocket(udp_);
			udp_ = INVALID_SOCKET;
			return;
		}
		setNonBlocking(udp_);
	}

	NetPeerId addPeer(SOCKET s) {
		NetPeerId id = nextId_++;
		Peer p;
		p.sock = s;
		peers_[id] = p;
		return id;
	}

	void onToken(NetPeerId id, unsigned long long token) {
		auto it = peers_.find(id);
		if (it == peers_.end()) return;
		it->second.token = token;
		if (!isServer_ && it->second.hasUdp) sendUdp(it->second, 0, 0);
	}

	bool parse(NetPeerId id, Peer& p) {
		for (;;) {
			if (p.in.size() < 5) return true;
			unsigned n = (unsigned char)p.in[0] |
				((unsigned char)p.in[1] << 8) |
				((unsigned char)p.in[2] << 16) |
				((unsigned char)p.in[3] << 24);
			if (n > (unsigned)DIRECT_MAX_FRAME) return false;
			if (p.in.size() < 5 + n) return true;
			unsigned char kind = (unsigned char)p.in[4];
			if (kind == FRAME_TOKEN) {
				if (n >= 8) onToken(id, readToken(&p.in[5]));
			} else {
				NetEvent ev = {};
				ev.type = NetEvent::Receive;
				ev.peer = id;
				ev.data.assign(p.in.begin() + 5, p.in.begin() + 5 + n);
				events_.push_back(ev);
			}
			p.in.erase(p.in.begin(), p.in.begin() + 5 + n);
		}
	}

	void killPeer(Peer& p) {
		if (p.dead) return;
		p.dead = true;
		if (p.sock != INVALID_SOCKET) {
			::closesocket(p.sock);
			p.sock = INVALID_SOCKET;
		}
	}

	void sendTcp(Peer& p, unsigned char kind, const void* data, int size) {
		if (p.dead) return;
		std::vector<char> f;
		appendFrame(f, kind, data, size);
		SOCKET s = p.sock;
		const char* ptr = f.data();
		int rem = (int)f.size();
		while (rem > 0) {
			fd_set wf;
			FD_ZERO(&wf);
			FD_SET(s, &wf);
			timeval tv = { 5, 0 };
			if (::select(0, 0, &wf, 0, &tv) <= 0) {
				// cut off the oxygen to stalled peers
				killPeer(p);
				return;
			}
			int n = ::send(s, ptr, rem, 0);
			if (n == SOCKET_ERROR && ::WSAGetLastError() == WSAEWOULDBLOCK) continue;
			if (n <= 0) {
				killPeer(p);
				return;
			}
			ptr += n;
			rem -= n;
		}
	}

	void sendUdp(Peer& p, const void* data, int size) {
		if (udp_ == INVALID_SOCKET || !p.token) return;
		std::vector<char> b;
		appendToken(b, p.token);
		if (size > 0) b.insert(b.end(), (const char*)data, (const char*)data + size);
		::sendto(udp_, b.data(), (int)b.size(), 0, (sockaddr*)&p.udpAddr, sizeof(p.udpAddr));
	}

	void pollUdp() {
		if (udp_ == INVALID_SOCKET) return;
		for (;;) {
			char buf[65536];
			sockaddr_in from = {};
			int flen = sizeof(from);
			int n = ::recvfrom(udp_, buf, sizeof(buf), 0, (sockaddr*)&from, &flen);
			if (n <= 0) break;
			if (n < 8) continue;
			unsigned long long token = readToken(buf);
			if (isServer_) {
				auto it = tokenToPeer_.find(token);
				if (it == tokenToPeer_.end()) continue;
				auto pit = peers_.find(it->second);
				if (pit == peers_.end()) continue;
				Peer& p = pit->second;
				if (!p.hasUdp) { p.udpAddr = from; p.hasUdp = true; }
				if (n > 8) {
					NetEvent ev = {};
					ev.type = NetEvent::Receive;
					ev.peer = it->second;
					ev.data.assign(buf + 8, buf + n);
					events_.push_back(ev);
				}
			} else {
				for (auto& kv : peers_) {
					if (kv.second.token != token) continue;
					if (n > 8) {
						NetEvent ev = {};
						ev.type = NetEvent::Receive;
						ev.peer = kv.first;
						ev.data.assign(buf + 8, buf + n);
						events_.push_back(ev);
					}
					break;
				}
			}
		}
	}

	SOCKET listener_;
	SOCKET udp_;
	SOCKET connectSock_ = INVALID_SOCKET;
	sockaddr_in connectAddr_ = {};
	std::shared_ptr<Resolve> resolve_;
	bool connecting_ = false;
	unsigned long long connectStartMs_ = 0;
	int maxPeers_ = 0;
	bool isServer_;
	std::map<NetPeerId, Peer> peers_;
	std::map<unsigned long long, NetPeerId> tokenToPeer_;
	std::vector<NetEvent> events_;
	NetPeerId nextId_;
	unsigned long long nextToken_;
	int port_;
};

INetTransport* createDirectTransport() {
	return new DirectTransport();
}
