#include "std.h"
#include "bbsys.h"
#include "bbnet.h"
#include "bbnet_transport.h"

#include <algorithm>

namespace {

	enum {
		NET_MSG_JOIN = 1,
		NET_MSG_WELCOME = 2,
		NET_MSG_ROSTER = 3,
		NET_MSG_USER = 4
	};

	const unsigned NET_TARGET_BROADCAST = 0xFFFFFFFFu;

	void put8(std::vector<char>& b, unsigned v) {
		b.push_back((char)(v & 0xff));
	}

	void put16(std::vector<char>& b, unsigned v) {
		put8(b, v);
		put8(b, v >> 8);
	}

	void put32(std::vector<char>& b, unsigned v) {
		put16(b, v);
		put16(b, v >> 16);
	}

	unsigned get8(const char* p) {
		return (unsigned char)p[0];
	}

	unsigned get16(const char* p) {
		return get8(p) | (get8(p + 1) << 8);
	}

	unsigned get32(const char* p) {
		return get16(p) | (get16(p + 2) << 16);
	}

	struct NetSession {
		INetTransport* transport;
		bool isHost;
		bool dedicated;
		bool connected;
		int localId;
		int authorityId;
		int backend;
		unsigned nextClientId;
		NetPeerId hostTransport;
		int lastSender;
		std::vector<int> roster;
		std::map<int, NetPeerId> sessionToTransport;
		std::map<NetPeerId, int> transportToSession;
		std::vector<std::pair<int, std::string>> messages;
		std::vector<std::string> events;

		NetSession()
			: transport(0), isHost(false), dedicated(false), connected(false),
			localId(-1), authorityId(-1), backend(0), nextClientId(1),
			hostTransport(0), lastSender(-1) {
		}

		~NetSession() {
			if (transport) {
				transport->close();
				delete transport;
			}
		}
	};

	std::unordered_set<NetSession*> net_set;

	void debugNet(NetSession* s, const char* function) {
		if (!net_set.count(s)) {
			ErrorLog(function, "Net Session does not exist");
		}
	}

	INetTransport* makeTransport(int backend) {
		if (backend == 0) return createDirectTransport();
		if (backend == 1) return createSteamTransport();
		return 0;
	}

	std::vector<char> buildRoster(NetSession* s) {
		std::vector<char> p;
		put32(p, (unsigned)s->authorityId);
		put16(p, (unsigned)s->roster.size());
		for (int id : s->roster) put32(p, (unsigned)id);
		return p;
	}

	void applyRoster(NetSession* s, const char* p, int len) {
		if (len < 6) return;
		s->authorityId = (int)get32(p);
		unsigned count = get16(p + 4);
		s->roster.clear();
		int off = 6;
		for (unsigned i = 0; i < count && off + 4 <= len; ++i, off += 4)
			s->roster.push_back((int)get32(p + off));
	}

	void sendEnvelope(NetSession* s, NetPeerId peer, unsigned char type, int sender, const std::vector<char>& payload) {
		std::vector<char> b;
		put8(b, type);
		put32(b, (unsigned)sender);
		put32(b, NET_TARGET_BROADCAST);
		b.insert(b.end(), payload.begin(), payload.end());
		s->transport->send(peer, b.data(), (int)b.size(), NetReliability::Reliable);
	}

	void sendUser(NetSession* s, NetPeerId peer, int sender, unsigned target, const std::string& data) {
		std::vector<char> b;
		put8(b, NET_MSG_USER);
		put32(b, (unsigned)sender);
		put32(b, target);
		b.insert(b.end(), data.begin(), data.end());
		s->transport->send(peer, b.data(), (int)b.size(), NetReliability::Reliable);
	}

	void hostDispatch(NetSession* s, int sender, unsigned target, const std::string& data, bool echo) {
		if (target == NET_TARGET_BROADCAST) {
			for (auto& kv : s->sessionToTransport)
				if (kv.first != sender) sendUser(s, kv.second, sender, target, data);
			if (echo) s->messages.push_back(std::make_pair(sender, data));
		} else if (target == (unsigned)s->localId) {
			s->messages.push_back(std::make_pair(sender, data));
		} else {
			auto it = s->sessionToTransport.find((int)target);
			if (it != s->sessionToTransport.end()) sendUser(s, it->second, sender, target, data);
		}
	}

	int processEvents(NetSession* s) {
		std::vector<NetEvent> evs = s->transport->drain();
		for (NetEvent& ev : evs) {
			if (ev.type == NetEvent::Connect) {
				if (s->isHost) {
					int sid = (int)s->nextClientId++;
					s->sessionToTransport[sid] = ev.peer;
					s->transportToSession[ev.peer] = sid;
					s->roster.push_back(sid);
					std::sort(s->roster.begin(), s->roster.end());
					std::vector<char> w;
					put32(w, (unsigned)sid);
					std::vector<char> rp = buildRoster(s);
					w.insert(w.end(), rp.begin(), rp.end());
					sendEnvelope(s, ev.peer, NET_MSG_WELCOME, s->localId, w);
					std::vector<char> rp2 = buildRoster(s);
					for (auto& kv : s->sessionToTransport)
						if (kv.second != ev.peer) sendEnvelope(s, kv.second, NET_MSG_ROSTER, s->localId, rp2);
					s->events.push_back("peer_join");
				} else {
					s->hostTransport = ev.peer;
					s->connected = true;
					std::vector<char> empty;
					sendEnvelope(s, s->hostTransport, NET_MSG_JOIN, 0, empty);
					s->events.push_back("connected");
				}
			} else if (ev.type == NetEvent::Disconnect) {
				if (s->isHost) {
					auto it = s->transportToSession.find(ev.peer);
					if (it != s->transportToSession.end()) {
						int sid = it->second;
						s->transportToSession.erase(it);
						s->sessionToTransport.erase(sid);
						s->roster.erase(std::remove(s->roster.begin(), s->roster.end(), sid), s->roster.end());
						std::vector<char> rp = buildRoster(s);
						for (auto& kv : s->sessionToTransport)
							sendEnvelope(s, kv.second, NET_MSG_ROSTER, s->localId, rp);
						s->events.push_back("peer_leave");
					}
				} else {
					s->connected = false;
					s->hostTransport = 0;
					if (s->localId >= 0) {
						s->roster.erase(std::remove(s->roster.begin(), s->roster.end(), s->authorityId), s->roster.end());
						if (!s->roster.empty()) {
							int newAuth = s->roster.front();
							for (int id : s->roster) if (id < newAuth) newAuth = id;
							s->authorityId = newAuth;
							s->events.push_back("authority_changed");
						} else {
							s->events.push_back("disconnected");
						}
					}
				}
			} else if (ev.type == NetEvent::Receive) {
				if (ev.data.size() < 9) continue;
				unsigned char type = (unsigned char)ev.data[0];
				int sender = (int)get32(&ev.data[1]);
				unsigned target = get32(&ev.data[5]);
				const char* payload = ev.data.data() + 9;
				int plen = (int)ev.data.size() - 9;
				if (type == NET_MSG_WELCOME) {
					if (plen >= 4) {
						s->localId = (int)get32(payload);
						applyRoster(s, payload + 4, plen - 4);
						s->events.push_back("welcome");
					}
				} else if (type == NET_MSG_ROSTER) {
					applyRoster(s, payload, plen);
				} else if (type == NET_MSG_USER) {
					std::string data(payload, payload + plen);
					if (s->isHost) hostDispatch(s, sender, target, data, true);
					else s->messages.push_back(std::make_pair(sender, data));
				}
			}
		}
		return (int)evs.size();
	}

}

static NetSession* bbNetCreateSession(int port, int dedicated, int backend) {
	if (port < 0 || port > 65535) RTEX("Invalid network port");
	INetTransport* t = makeTransport(backend);
	if (!t) RTEX("Network backend is unavailable");
	if (!t->listen(port)) {
		delete t;
		RTEX("Failed to create listen session");
	}
	NetSession* s = new NetSession();
	s->transport = t;
	s->isHost = true;
	s->dedicated = dedicated != 0;
	s->backend = backend;
	s->localId = 0;
	s->authorityId = 0;
	s->roster.push_back(0);
	net_set.insert(s);
	return s;
}

static NetSession* bbNetJoinSession(BBStr* host, int port, int backend) {
	std::string h = *host;
	delete host;
	if (port < 0 || port > 65535) RTEX("Invalid network port");
	INetTransport* t = makeTransport(backend);
	if (!t) RTEX("Network backend is unavailable");
	if (!t->connect(h.c_str(), port)) {
		delete t;
		RTEX("Failed to connect to session");
	}
	NetSession* s = new NetSession();
	s->transport = t;
	s->backend = backend;
	s->authorityId = 0;
	net_set.insert(s);
	t->poll();
	processEvents(s);
	return s;
}

static void bbNetCloseSession(NetSession* s) {
	debugNet(s, "CloseNetSession");
	net_set.erase(s);
	delete s;
}

static int bbNetPoll(NetSession* s) {
	debugNet(s, "NetPoll");
	s->transport->poll();
	return processEvents(s);
}

static int bbNetBackend(NetSession* s) {
	debugNet(s, "NetBackend");
	return s->backend;
}

static int bbNetSessionId(NetSession* s) {
	debugNet(s, "NetSessionId");
	return s->localId;
}

static int bbNetAuthorityId(NetSession* s) {
	debugNet(s, "NetAuthorityId");
	return s->authorityId;
}

static int bbNetIsAuthority(NetSession* s) {
	debugNet(s, "NetIsAuthority");
	return s->localId >= 0 && s->localId == s->authorityId;
}

static int bbNetConnected(NetSession* s) {
	debugNet(s, "NetConnected");
	return (s->connected || s->isHost) ? 1 : 0;
}

static int bbNetPeerCount(NetSession* s) {
	debugNet(s, "NetPeerCount");
	return (int)s->roster.size();
}

static int bbNetPeerId(NetSession* s, int index) {
	debugNet(s, "NetPeerId");
	if (index < 1 || index >(int)s->roster.size()) RTEX("Net peer index out of range");
	return s->roster[index - 1];
}

static void bbNetSend(NetSession* s, int peer, BBStr* data) {
	debugNet(s, "NetSend");
	std::string d = *data;
	delete data;
	if (s->isHost) {
		hostDispatch(s, s->localId, (unsigned)peer, d, true);
	} else {
		if (!s->connected) RTEX("Not connected to a net session");
		sendUser(s, s->hostTransport, s->localId, (unsigned)peer, d);
		if (peer == s->localId) s->messages.push_back(std::make_pair(s->localId, d));
	}
}

static void bbNetBroadcast(NetSession* s, BBStr* data) {
	debugNet(s, "NetBroadcast");
	std::string d = *data;
	delete data;
	if (s->isHost) {
		hostDispatch(s, s->localId, NET_TARGET_BROADCAST, d, true);
	} else {
		if (!s->connected) RTEX("Not connected to a net session");
		sendUser(s, s->hostTransport, s->localId, NET_TARGET_BROADCAST, d);
		s->messages.push_back(std::make_pair(s->localId, d));
	}
}

static BBStr* bbNetRecv(NetSession* s) {
	debugNet(s, "NetRecv");
	if (s->messages.empty()) return new BBStr("");
	std::pair<int, std::string> m = s->messages.front();
	s->messages.erase(s->messages.begin());
	s->lastSender = m.first;
	return new BBStr(m.second);
}

static int bbNetMsgSender(NetSession* s) {
	debugNet(s, "NetMsgSender");
	return s->lastSender;
}

static BBStr* bbNetEvent(NetSession* s) {
	debugNet(s, "NetEvent");
	if (s->events.empty()) return new BBStr("");
	std::string e = s->events.front();
	s->events.erase(s->events.begin());
	return new BBStr(e);
}

bool net_create() {
	return true;
}

bool net_destroy() {
	while (!net_set.empty()) bbNetCloseSession(*net_set.begin());
	return true;
}

void net_link(void (*rtSym)(const char* sym, void* pc)) {
	rtSym("%NetCreateSession%port%dedicated=0%backend=0", bbNetCreateSession);
	rtSym("%NetJoinSession$host%port%backend=0", bbNetJoinSession);
	rtSym("CloseNetSession%session", bbNetCloseSession);
	rtSym("%NetPoll%session", bbNetPoll);
	rtSym("%NetBackend%session", bbNetBackend);
	rtSym("%NetSessionId%session", bbNetSessionId);
	rtSym("%NetAuthorityId%session", bbNetAuthorityId);
	rtSym("%NetIsAuthority%session", bbNetIsAuthority);
	rtSym("%NetConnected%session", bbNetConnected);
	rtSym("%NetPeerCount%session", bbNetPeerCount);
	rtSym("%NetPeerId%session%index", bbNetPeerId);
	rtSym("NetSend%session%peer$data", bbNetSend);
	rtSym("NetBroadcast%session$data", bbNetBroadcast);
	rtSym("$NetRecv%session", bbNetRecv);
	rtSym("%NetMsgSender%session", bbNetMsgSender);
	rtSym("$NetEvent%session", bbNetEvent);
}
