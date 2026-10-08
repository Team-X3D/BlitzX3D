#ifndef BBNET_TRANSPORT_H
#define BBNET_TRANSPORT_H

#include "std.h"

using NetPeerId = unsigned long long;

enum class NetReliability { Unreliable, Reliable };

struct NetEvent {
	enum Type { Connect, Disconnect, Receive } type;
	NetPeerId peer;
	std::vector<char> data;
};

class INetTransport {
public:
	virtual ~INetTransport() {}

	virtual const char* name() const = 0;

	virtual bool listen(int port) = 0;
	virtual bool connect(const char* host, int port) = 0;

	virtual void poll() = 0;

	virtual void send(NetPeerId to, const void* data, int size, NetReliability reliability) = 0;
	virtual void broadcast(const void* data, int size, NetReliability reliability) = 0;
	virtual void closePeer(NetPeerId id) = 0;
	virtual void close() = 0;

	virtual std::vector<NetEvent> drain() = 0;

	virtual int hostPort() const = 0;
};

INetTransport* createDirectTransport();
INetTransport* createSteamTransport();

#endif
