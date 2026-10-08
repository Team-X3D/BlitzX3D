#include "std.h"
#include "bbsys.h"
#include "bbnet.h"
#include "bbnet_transport.h"

#include <algorithm>
#include <cstring>

namespace {

	enum {
		NET_MSG_JOIN = 1,
		NET_MSG_WELCOME = 2,
		NET_MSG_ROSTER = 3,
		NET_MSG_USER = 4,
		NET_MSG_OBJ_ADD = 5,
		NET_MSG_OBJ_DEL = 6,
		NET_MSG_OWNER = 7,
		NET_MSG_SNAP = 8
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

	void putF32(std::vector<char>& b, float f) {
		unsigned u;
		memcpy(&u, &f, 4);
		put32(b, u);
	}

	float getF32(const char* p) {
		unsigned u = get32(p);
		float f;
		memcpy(&f, &u, 4);
		return f;
	}

	struct NetObject {
		int owner;
		bool valid;
		float x, y, z;
		float yaw, pitch, roll;
		float px, py, pz;
		float pyaw, ppitch, proll;
		unsigned long long prevMs;
		unsigned long long targetMs;
		bool hasPrev;
		bool hasTarget;
		bool sentOnce;
		bool dirtyPos;
		bool dirtyRot;
		bool lastPos;
		bool lastRot;
		unsigned long long resendUntil;

		NetObject()
			: owner(-1), valid(false),
			x(0), y(0), z(0), yaw(0), pitch(0), roll(0),
			px(0), py(0), pz(0), pyaw(0), ppitch(0), proll(0),
			prevMs(0), targetMs(0), hasPrev(false), hasTarget(false),
			sentOnce(false), dirtyPos(false), dirtyRot(false),
			lastPos(false), lastRot(false), resendUntil(0) {
		}
	};

	struct NetSession {
		INetTransport* transport;
		bool isHost;
		bool dedicated;
		bool connected;
		int localId;
		int authorityId;
		unsigned nextClientId;
		NetPeerId hostTransport;
		int lastSender;
		std::vector<int> roster;
		std::map<int, NetPeerId> sessionToTransport;
		std::map<NetPeerId, int> transportToSession;
		std::vector<std::pair<int, std::string>> messages;
		std::vector<std::string> events;
		std::map<int, NetObject> objects;
		int tickHz;
		int tickMs;
		int resendMs;
		unsigned long long lastTickMs;

		NetSession()
			: transport(0), isHost(false), dedicated(false), connected(false),
			localId(-1), authorityId(-1), nextClientId(1),
			hostTransport(0), lastSender(-1),
			tickHz(20), tickMs(50), resendMs(150), lastTickMs(0) {
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

	void sendUser(NetSession* s, NetPeerId peer, int sender, unsigned target, const std::string& data, bool reliable) {
		std::vector<char> b;
		put8(b, NET_MSG_USER);
		put32(b, (unsigned)sender);
		put32(b, target);
		put8(b, reliable ? 1 : 0);
		b.insert(b.end(), data.begin(), data.end());
		s->transport->send(peer, b.data(), (int)b.size(),
			reliable ? NetReliability::Reliable : NetReliability::Unreliable);
	}

	void hostDispatch(NetSession* s, int sender, unsigned target, const std::string& data, bool echo, bool reliable) {
		if (target == NET_TARGET_BROADCAST) {
			for (auto& kv : s->sessionToTransport)
				if (kv.first != sender) sendUser(s, kv.second, sender, target, data, reliable);
			if (echo) s->messages.push_back(std::make_pair(sender, data));
		} else if (target == (unsigned)s->localId) {
			s->messages.push_back(std::make_pair(sender, data));
		} else {
			auto it = s->sessionToTransport.find((int)target);
			if (it != s->sessionToTransport.end()) sendUser(s, it->second, sender, target, data, reliable);
		}
	}

	void sendSnapshot(NetSession* s, NetPeerId peer, const std::vector<char>& payload, bool reliable) {
		if (payload.size() < 2) return;
		std::vector<char> b;
		put8(b, NET_MSG_SNAP);
		put32(b, (unsigned)s->localId);
		put32(b, NET_TARGET_BROADCAST);
		b.insert(b.end(), payload.begin(), payload.end());
		s->transport->send(peer, b.data(), (int)b.size(),
			reliable ? NetReliability::Reliable : NetReliability::Unreliable);
	}

	std::vector<char> buildSnapshot(NetSession* s, const std::vector<int>& ids, bool full, unsigned long long nowMs) {
		std::vector<char> out;
		put16(out, 0);
		unsigned count = 0;
		for (int id : ids) {
			auto it = s->objects.find(id);
			if (it == s->objects.end()) continue;
			NetObject& o = it->second;
			bool sendPos, sendRot;
			if (full || !o.sentOnce) {
				sendPos = sendRot = true;
			} else if (o.dirtyPos || o.dirtyRot) {
				sendPos = o.dirtyPos;
				sendRot = o.dirtyRot;
			} else if (nowMs < o.resendUntil) {
				sendPos = o.lastPos;
				sendRot = o.lastRot;
			} else {
				continue;
			}
			if (!sendPos && !sendRot) continue;
			unsigned char flags = 0;
			if (sendPos) flags |= 1;
			if (sendRot) flags |= 2;
			put32(out, (unsigned)id);
			put8(out, flags);
			if (sendPos) { putF32(out, o.x); putF32(out, o.y); putF32(out, o.z); }
			if (sendRot) { putF32(out, o.yaw); putF32(out, o.pitch); putF32(out, o.roll); }
			++count;
			o.sentOnce = true;
			o.lastPos = sendPos;
			o.lastRot = sendRot;
			if (!full) {
				o.resendUntil = nowMs + s->resendMs;
				o.dirtyPos = false;
				o.dirtyRot = false;
			}
		}
		out[0] = (char)(count & 0xff);
		out[1] = (char)((count >> 8) & 0xff);
		return out;
	}

	void applySnapshot(NetSession* s, const char* p, int len, int sender, unsigned long long nowMs) {
		if (len < 2) return;
		unsigned count = get16(p);
		int off = 2;
		for (unsigned i = 0; i < count; ++i) {
			if (off + 5 > len) break;
			int id = (int)get32(p + off);
			off += 4;
			unsigned char flags = (unsigned char)p[off++];
			NetObject& o = s->objects[id];
			if (!o.valid) {
				o.valid = true;
				o.owner = sender;
			}
			if (o.hasTarget) {
				o.px = o.x; o.py = o.y; o.pz = o.z;
				o.pyaw = o.yaw; o.ppitch = o.pitch; o.proll = o.roll;
				o.prevMs = o.targetMs;
				o.hasPrev = true;
			}
			if ((flags & 1) && off + 12 <= len) {
				o.x = getF32(p + off);
				o.y = getF32(p + off + 4);
				o.z = getF32(p + off + 8);
				off += 12;
			}
			if ((flags & 2) && off + 12 <= len) {
				o.yaw = getF32(p + off);
				o.pitch = getF32(p + off + 4);
				o.roll = getF32(p + off + 8);
				off += 12;
			}
			o.targetMs = nowMs;
			o.hasTarget = true;
		}
	}

	void replicateTick(NetSession* s) {
		if (!s->isHost && !s->connected) return;
		unsigned long long now = GetTickCount64();
		if (s->lastTickMs == 0) s->lastTickMs = now;
		if (now - s->lastTickMs < (unsigned long long)s->tickMs) return;
		s->lastTickMs = now;
		std::vector<int> ids;
		for (auto& kv : s->objects)
			if (kv.second.valid && kv.second.owner == s->localId) ids.push_back(kv.first);
		if (ids.empty()) return;
		std::vector<char> snap = buildSnapshot(s, ids, false, now);
		if (snap.size() < 3) return;
		if (s->isHost) {
			for (auto& kv : s->sessionToTransport) sendSnapshot(s, kv.second, snap, false);
		} else if (s->hostTransport) {
			sendSnapshot(s, s->hostTransport, snap, false);
		}
	}

	int processEvents(NetSession* s) {
		unsigned long long now = GetTickCount64();
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
					std::vector<int> all;
					for (auto& kv : s->objects) if (kv.second.valid) all.push_back(kv.first);
					if (!all.empty()) sendSnapshot(s, ev.peer, buildSnapshot(s, all, true, now), true);
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
					bool reliable = plen >= 1 && payload[0] != 0;
					std::string data;
					if (plen > 1) data.assign(payload + 1, payload + plen);
					if (s->isHost) hostDispatch(s, sender, target, data, true, reliable);
					else s->messages.push_back(std::make_pair(sender, data));
				} else if (type == NET_MSG_OBJ_ADD) {
					if (plen >= 8) {
						int id = (int)get32(payload);
						int owner = (int)get32(payload + 4);
						NetObject& o = s->objects[id];
						o.valid = true;
						o.owner = owner;
						o.dirtyPos = true;
						o.dirtyRot = true;
					}
				} else if (type == NET_MSG_OBJ_DEL) {
					if (plen >= 4) s->objects.erase((int)get32(payload));
				} else if (type == NET_MSG_OWNER) {
					if (plen >= 8) {
						int id = (int)get32(payload);
						auto it = s->objects.find(id);
						if (it != s->objects.end()) it->second.owner = (int)get32(payload + 4);
					}
				} else if (type == NET_MSG_SNAP) {
					applySnapshot(s, payload, plen, sender, now);
					if (s->isHost) {
						std::vector<char> fwd;
						put8(fwd, NET_MSG_SNAP);
						put32(fwd, (unsigned)sender);
						put32(fwd, NET_TARGET_BROADCAST);
						fwd.insert(fwd.end(), payload, payload + plen);
						for (auto& kv : s->sessionToTransport)
							if (kv.first != sender)
								s->transport->send(kv.second, fwd.data(), (int)fwd.size(), NetReliability::Unreliable);
					}
				}
			}
		}
		return (int)evs.size();
	}

}

static NetSession* bbNetCreateSession(int port, int dedicated) {
	if (port < 0 || port > 65535) RTEX("Invalid network port");
	INetTransport* t = createDirectTransport();
	if (!t->listen(port)) {
		delete t;
		RTEX("Failed to create listen session");
	}
	NetSession* s = new NetSession();
	s->transport = t;
	s->isHost = true;
	s->dedicated = dedicated != 0;
	s->localId = 0;
	s->authorityId = 0;
	s->roster.push_back(0);
	net_set.insert(s);
	return s;
}

static NetSession* bbNetJoinSession(BBStr* host, int port) {
	std::string h = *host;
	delete host;
	if (port < 0 || port > 65535) RTEX("Invalid network port");
	INetTransport* t = createDirectTransport();
	if (!t->connect(h.c_str(), port)) {
		delete t;
		RTEX("Failed to connect to session");
	}
	NetSession* s = new NetSession();
	s->transport = t;
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
	int n = processEvents(s);
	replicateTick(s);
	return n;
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

static void bbNetSend(NetSession* s, int peer, BBStr* data, int reliable) {
	debugNet(s, "NetSend");
	std::string d = *data;
	delete data;
	bool rel = reliable != 0;
	if (s->isHost) {
		hostDispatch(s, s->localId, (unsigned)peer, d, true, rel);
	} else {
		if (!s->connected) RTEX("Not connected to a net session");
		sendUser(s, s->hostTransport, s->localId, (unsigned)peer, d, rel);
		if (peer == s->localId) s->messages.push_back(std::make_pair(s->localId, d));
	}
}

static void bbNetBroadcast(NetSession* s, BBStr* data, int reliable) {
	debugNet(s, "NetBroadcast");
	std::string d = *data;
	delete data;
	bool rel = reliable != 0;
	if (s->isHost) {
		hostDispatch(s, s->localId, NET_TARGET_BROADCAST, d, true, rel);
	} else {
		if (!s->connected) RTEX("Not connected to a net session");
		sendUser(s, s->hostTransport, s->localId, NET_TARGET_BROADCAST, d, rel);
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

static float objectComponent(NetSession* s, int netId, int comp) {
	auto it = s->objects.find(netId);
	if (it == s->objects.end()) return 0.0f;
	NetObject& o = it->second;
	float cur = comp == 0 ? o.x : comp == 1 ? o.y : comp == 2 ? o.z :
		comp == 3 ? o.yaw : comp == 4 ? o.pitch : o.roll;
	if (o.owner == s->localId || !o.hasTarget || !o.hasPrev) return cur;
	if (o.targetMs <= o.prevMs) return cur;
	float t = (float)(GetTickCount64() - o.prevMs) / (float)(o.targetMs - o.prevMs);
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	float prev = comp == 0 ? o.px : comp == 1 ? o.py : comp == 2 ? o.pz :
		comp == 3 ? o.pyaw : comp == 4 ? o.ppitch : o.proll;
	return prev + (cur - prev) * t;
}

static int bbNetSetTickRate(NetSession* s, int hz) {
	debugNet(s, "NetSetTickRate");
	if (hz < 1) hz = 1;
	if (hz > 1000) hz = 1000;
	s->tickHz = hz;
	s->tickMs = 1000 / hz;
	if (s->tickMs < 1) s->tickMs = 1;
	s->resendMs = s->tickMs * 3;
	return hz;
}

static int bbNetRegisterObject(NetSession* s, int netId) {
	debugNet(s, "NetRegisterObject");
	if (!s->isHost) return 0;
	if (s->objects.count(netId)) return 0;
	NetObject& o = s->objects[netId];
	o.valid = true;
	o.owner = s->localId;
	o.dirtyPos = true;
	o.dirtyRot = true;
	std::vector<char> p;
	put32(p, (unsigned)netId);
	put32(p, (unsigned)s->localId);
	for (auto& kv : s->sessionToTransport) sendEnvelope(s, kv.second, NET_MSG_OBJ_ADD, s->localId, p);
	return 1;
}

static void bbNetUnregisterObject(NetSession* s, int netId) {
	debugNet(s, "NetUnregisterObject");
	if (!s->isHost) return;
	s->objects.erase(netId);
	std::vector<char> p;
	put32(p, (unsigned)netId);
	for (auto& kv : s->sessionToTransport) sendEnvelope(s, kv.second, NET_MSG_OBJ_DEL, s->localId, p);
}

static int bbNetObjectExists(NetSession* s, int netId) {
	debugNet(s, "NetObjectExists");
	auto it = s->objects.find(netId);
	return (it != s->objects.end() && it->second.valid) ? 1 : 0;
}

static int bbNetSetObjectOwner(NetSession* s, int netId, int peer) {
	debugNet(s, "NetSetObjectOwner");
	if (!s->isHost) return 0;
	auto it = s->objects.find(netId);
	if (it == s->objects.end()) return 0;
	it->second.owner = peer;
	std::vector<char> p;
	put32(p, (unsigned)netId);
	put32(p, (unsigned)peer);
	for (auto& kv : s->sessionToTransport) sendEnvelope(s, kv.second, NET_MSG_OWNER, s->localId, p);
	return 1;
}

static int bbNetObjectOwner(NetSession* s, int netId) {
	debugNet(s, "NetObjectOwner");
	auto it = s->objects.find(netId);
	return it == s->objects.end() ? -1 : it->second.owner;
}

static void bbNetSetObjectPosition(NetSession* s, int netId, float x, float y, float z) {
	debugNet(s, "NetSetObjectPosition");
	auto it = s->objects.find(netId);
	if (it == s->objects.end()) return;
	NetObject& o = it->second;
	if (o.owner != s->localId) return;
	o.x = x;
	o.y = y;
	o.z = z;
	o.dirtyPos = true;
	o.valid = true;
}

static void bbNetSetObjectRotation(NetSession* s, int netId, float yaw, float pitch, float roll) {
	debugNet(s, "NetSetObjectRotation");
	auto it = s->objects.find(netId);
	if (it == s->objects.end()) return;
	NetObject& o = it->second;
	if (o.owner != s->localId) return;
	o.yaw = yaw;
	o.pitch = pitch;
	o.roll = roll;
	o.dirtyRot = true;
	o.valid = true;
}

static float bbNetObjectX(NetSession* s, int netId) { debugNet(s, "NetObjectX"); return objectComponent(s, netId, 0); }
static float bbNetObjectY(NetSession* s, int netId) { debugNet(s, "NetObjectY"); return objectComponent(s, netId, 1); }
static float bbNetObjectZ(NetSession* s, int netId) { debugNet(s, "NetObjectZ"); return objectComponent(s, netId, 2); }
static float bbNetObjectYaw(NetSession* s, int netId) { debugNet(s, "NetObjectYaw"); return objectComponent(s, netId, 3); }
static float bbNetObjectPitch(NetSession* s, int netId) { debugNet(s, "NetObjectPitch"); return objectComponent(s, netId, 4); }
static float bbNetObjectRoll(NetSession* s, int netId) { debugNet(s, "NetObjectRoll"); return objectComponent(s, netId, 5); }

static int bbNetObjectCount(NetSession* s) {
	debugNet(s, "NetObjectCount");
	return (int)s->objects.size();
}

static int bbNetObjectId(NetSession* s, int index) {
	debugNet(s, "NetObjectId");
	if (index < 1 || index >(int)s->objects.size()) RTEX("Net object index out of range");
	int i = 1;
	for (auto& kv : s->objects) {
		if (i == index) return kv.first;
		++i;
	}
	return 0;
}

static int bbNetObjectIsMine(NetSession* s, int netId) {
	debugNet(s, "NetObjectIsMine");
	auto it = s->objects.find(netId);
	return (it != s->objects.end() && it->second.owner == s->localId) ? 1 : 0;
}

bool net_create() {
	return true;
}

bool net_destroy() {
	while (!net_set.empty()) bbNetCloseSession(*net_set.begin());
	return true;
}

void net_link(void (*rtSym)(const char* sym, void* pc)) {
	rtSym("%NetCreateSession%port%dedicated=0", bbNetCreateSession);
	rtSym("%NetJoinSession$host%port", bbNetJoinSession);
	rtSym("CloseNetSession%session", bbNetCloseSession);
	rtSym("%NetPoll%session", bbNetPoll);
	rtSym("%NetSessionId%session", bbNetSessionId);
	rtSym("%NetAuthorityId%session", bbNetAuthorityId);
	rtSym("%NetIsAuthority%session", bbNetIsAuthority);
	rtSym("%NetConnected%session", bbNetConnected);
	rtSym("%NetPeerCount%session", bbNetPeerCount);
	rtSym("%NetPeerId%session%index", bbNetPeerId);
	rtSym("NetSend%session%peer$data%reliable=1", bbNetSend);
	rtSym("NetBroadcast%session$data%reliable=1", bbNetBroadcast);
	rtSym("$NetRecv%session", bbNetRecv);
	rtSym("%NetMsgSender%session", bbNetMsgSender);
	rtSym("$NetEvent%session", bbNetEvent);
	rtSym("%NetSetTickRate%session%hz", bbNetSetTickRate);
	rtSym("%NetRegisterObject%session%net_id", bbNetRegisterObject);
	rtSym("NetUnregisterObject%session%net_id", bbNetUnregisterObject);
	rtSym("%NetObjectExists%session%net_id", bbNetObjectExists);
	rtSym("%NetSetObjectOwner%session%net_id%peer", bbNetSetObjectOwner);
	rtSym("%NetObjectOwner%session%net_id", bbNetObjectOwner);
	rtSym("NetSetObjectPosition%session%net_id#x#y#z", bbNetSetObjectPosition);
	rtSym("NetSetObjectRotation%session%net_id#yaw#pitch#roll", bbNetSetObjectRotation);
	rtSym("#NetObjectX%session%net_id", bbNetObjectX);
	rtSym("#NetObjectY%session%net_id", bbNetObjectY);
	rtSym("#NetObjectZ%session%net_id", bbNetObjectZ);
	rtSym("#NetObjectYaw%session%net_id", bbNetObjectYaw);
	rtSym("#NetObjectPitch%session%net_id", bbNetObjectPitch);
	rtSym("#NetObjectRoll%session%net_id", bbNetObjectRoll);
	rtSym("%NetObjectCount%session", bbNetObjectCount);
	rtSym("%NetObjectId%session%index", bbNetObjectId);
	rtSym("%NetObjectIsMine%session%net_id", bbNetObjectIsMine);
}
