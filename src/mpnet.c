/** @file src/mpnet.c The socket that carries turn packets to the relay. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
	#include <winsock2.h>
	#include <ws2tcpip.h>
	typedef SOCKET MpSocket;
	#define MP_SOCKET_INVALID INVALID_SOCKET
	#define MpSocket_Close(s) closesocket(s)
	#define MpSocket_WouldBlock() (WSAGetLastError() == WSAEWOULDBLOCK)
#else
	#include <errno.h>
	#include <fcntl.h>
	#include <netdb.h>
	#include <netinet/in.h>
	#include <netinet/tcp.h>
	#include <sys/socket.h>
	#include <unistd.h>
	typedef int MpSocket;
	#define MP_SOCKET_INVALID (-1)
	#define MpSocket_Close(s) close(s)
	#define MpSocket_WouldBlock() (errno == EAGAIN || errno == EWOULDBLOCK)
#endif /* _WIN32 */

#include "types.h"
#include "os/common.h"
#include "os/sleep.h"

#include "mpnet.h"

#include "house.h"
#include "match.h"
#include "mpturn.h"

/* Deep enough that a client can fall a few turns behind without losing a
 * packet, and shallow enough to cost nothing: only the turns between the one
 * being applied and the one being filled are ever asked for. */
enum {
	MP_NET_WINDOW  = 64,
	MP_NET_BUFFER  = 65536,
	MP_NET_PAYLOAD = 8192
};

static struct {
	bool connected;
	bool ready;
	bool left;
	uint8 leftSlot;
	uint8 localSlot;
	MpSocket socket;
	char error[160];

	/* Everything read from the socket and not yet parsed.  TCP is a stream, so
	 * a packet may arrive in pieces, and two may arrive as one read. */
	char in[MP_NET_BUFFER];
	uint32 inUsed;

	struct {
		bool used;
		uint32 turn;
		MpPacket packet;
	} window[MATCH_SLOT_MAX][MP_NET_WINDOW];
} s_net;

static void MpNet_Fail(const char *what)
{
	snprintf(s_net.error, sizeof(s_net.error), "%s", what);
}

const char *MpNet_GetError(void)
{
	return s_net.error;
}

bool MpNet_IsConnected(void)
{
	return s_net.connected;
}

bool MpNet_IsReady(void)
{
	return s_net.ready;
}

bool MpNet_HasLeft(uint8 *slot)
{
	if (slot != NULL) *slot = s_net.leftSlot;

	return s_net.left;
}

/**
 * Dial the relay and claim a slot in a room.
 *
 * The join is done with the socket still blocking, because there is nothing to
 * do until it succeeds and a half-open connection is worse than a slow one.
 * Everything after that is non-blocking: the game loop may never wait on the
 * network anywhere except the deliberate stall in MpTurn_Advance().
 */
bool MpNet_Connect(const char *host, uint16 port, const char *room, uint8 slot)
{
	struct addrinfo hints;
	struct addrinfo *results = NULL;
	struct addrinfo *entry;
	char service[16];
	char request[128];
	MpSocket sock = MP_SOCKET_INVALID;
	uint16 attempt;
	int one = 1;

#if defined(_WIN32)
	WSADATA wsa;

	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		MpNet_Fail("could not start winsock");
		return false;
	}
#endif /* _WIN32 */

	memset(&s_net, 0, sizeof(s_net));
	s_net.socket    = MP_SOCKET_INVALID;
	s_net.localSlot = slot;

	snprintf(service, sizeof(service), "%u", (unsigned)port);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(host, service, &hints, &results) != 0 || results == NULL) {
		MpNet_Fail("could not resolve the relay address");
		return false;
	}

	/* Three attempts, because a single refused connect is not evidence of
	 * anything.  Transparent proxies and carrier middleboxes drop the odd
	 * outbound SYN, and on this machine two clients dialling the same relay at
	 * the same moment lose one often enough to be annoying -- reported to the
	 * player as "could not reach the relay", which sends them looking at the
	 * server instead of at their own network. */
	for (attempt = 0; attempt < 3 && sock == MP_SOCKET_INVALID; attempt++) {
		if (attempt != 0) msleep(250);

		for (entry = results; entry != NULL; entry = entry->ai_next) {
			sock = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
			if (sock == MP_SOCKET_INVALID) continue;

			if (connect(sock, entry->ai_addr, (int)entry->ai_addrlen) == 0) break;

			MpSocket_Close(sock);
			sock = MP_SOCKET_INVALID;
		}
	}

	freeaddrinfo(results);

	if (sock == MP_SOCKET_INVALID) {
		MpNet_Fail("could not reach the relay");
		return false;
	}

	/* Every turn is one small packet the other player is already waiting for, so
	 * waiting to fill a segment is exactly the wrong trade. */
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

	snprintf(request, sizeof(request), "JOIN %s %u\n", room, (unsigned)slot);

	if (send(sock, request, (int)strlen(request), 0) < 0) {
		MpSocket_Close(sock);
		MpNet_Fail("could not send the join request");
		return false;
	}

#if defined(_WIN32)
	{
		u_long mode = 1;
		ioctlsocket(sock, FIONBIO, &mode);
	}
#else
	fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif /* _WIN32 */

	s_net.socket    = sock;
	s_net.connected = true;

	return true;
}

void MpNet_Disconnect(void)
{
	if (s_net.socket != MP_SOCKET_INVALID) MpSocket_Close(s_net.socket);

	s_net.socket    = MP_SOCKET_INVALID;
	s_net.connected = false;

#if defined(_WIN32)
	WSACleanup();
#endif /* _WIN32 */
}

/** Put a received packet where poll() will find it. */
static void MpNet_Store(uint8 slot, const MpPacket *packet)
{
	uint32 i;

	if (slot >= MATCH_SLOT_MAX) return;

	i = packet->turn % MP_NET_WINDOW;

	s_net.window[slot][i].packet = *packet;
	s_net.window[slot][i].turn   = packet->turn;
	s_net.window[slot][i].used   = true;
}

/**
 * Consume as many whole messages as the buffer holds.
 *
 * @return How many bytes were consumed.
 */
static uint32 MpNet_ParseBuffer(void)
{
	uint32 consumed = 0;

	while (true) {
		char *start = s_net.in + consumed;
		uint32 left = s_net.inUsed - consumed;
		char *newline;
		uint32 headerLength;
		unsigned slot;
		unsigned length;

		if (left == 0) break;

		newline = (char *)memchr(start, '\n', left);
		if (newline == NULL) break;

		headerLength = (uint32)(newline - start) + 1;

		if (strncmp(start, "PKT ", 4) == 0) {
			MpPacket packet;
			char body[MP_NET_PAYLOAD + 1];

			if (sscanf(start, "PKT %u %u", &slot, &length) != 2) {
				MpNet_Fail("the relay sent a malformed packet header");
				return consumed + headerLength;
			}

			if (length > MP_NET_PAYLOAD) {
				MpNet_Fail("the relay sent an oversized packet");
				return consumed + headerLength;
			}

			/* The body has not all arrived yet: leave the whole message in the
			 * buffer and try again after the next read. */
			if (left < headerLength + length) break;

			memcpy(body, start + headerLength, length);
			body[length] = '\0';

			if (MpPacket_Parse(body, &packet)) MpNet_Store((uint8)slot, &packet);

			consumed += headerLength + length;
			continue;
		}

		if (strncmp(start, "READY", 5) == 0) {
			s_net.ready = true;
		} else if (strncmp(start, "LEFT ", 5) == 0) {
			if (sscanf(start, "LEFT %u", &slot) == 1) {
				s_net.left     = true;
				s_net.leftSlot = (uint8)slot;
			}
		} else if (strncmp(start, "ERROR ", 6) == 0) {
			snprintf(s_net.error, sizeof(s_net.error), "%.*s", (int)(headerLength - 7), start + 6);
		}
		/* WELCOME needs nothing: the slot is what we asked for, and the count is
		 * only interesting to a lobby. */

		consumed += headerLength;
	}

	return consumed;
}

/**
 * Drain the socket.
 *
 * Called from both halves of the transport, so a client that is only sending
 * still notices packets, a disconnection and the relay's own messages.
 */
void MpNet_Pump(void)
{
	if (!s_net.connected) return;

	while (true) {
		uint32 consumed;
		int got;

		if (s_net.inUsed < sizeof(s_net.in)) {
			got = (int)recv(s_net.socket, s_net.in + s_net.inUsed, (int)(sizeof(s_net.in) - s_net.inUsed), 0);

			if (got > 0) {
				s_net.inUsed += (uint32)got;
			} else if (got == 0) {
				MpNet_Fail("the relay closed the connection");
				s_net.connected = false;
				return;
			} else if (!MpSocket_WouldBlock()) {
				MpNet_Fail("the connection to the relay broke");
				s_net.connected = false;
				return;
			}
		} else {
			got = 0;
		}

		consumed = MpNet_ParseBuffer();

		if (consumed != 0) {
			memmove(s_net.in, s_net.in + consumed, s_net.inUsed - consumed);
			s_net.inUsed -= consumed;
		}

		/* Nothing new to read and nothing left to parse. */
		if (got <= 0 && consumed == 0) break;
	}
}

static bool MpTransport_Net_Send(uint8 slot, const MpPacket *packet)
{
	char body[MP_NET_PAYLOAD];
	char frame[MP_NET_PAYLOAD + 64];
	uint16 used;
	uint32 total;
	uint32 sent = 0;

	MpNet_Pump();

	if (!s_net.connected) return false;

	/* Our own packets go straight into the window rather than round trip through
	 * the relay: we are one of the players, and waiting to hear our own move
	 * back would put a whole ping into every turn for no reason. */
	MpNet_Store(slot, packet);

	if (slot != s_net.localSlot) return true;

	used  = MpPacket_Format(body, sizeof(body), packet);
	total = (uint32)snprintf(frame, sizeof(frame), "PKT %u\n", (unsigned)used);

	memcpy(frame + total, body, used);
	total += used;

	while (sent < total) {
		int wrote = (int)send(s_net.socket, frame + sent, (int)(total - sent), 0);

		if (wrote > 0) {
			sent += (uint32)wrote;
			continue;
		}

		if (wrote < 0 && MpSocket_WouldBlock()) {
			/* The relay is behind on reading.  There is nothing useful to do
			 * with the time -- the turn cannot close until this is out. */
			MpNet_Pump();
			continue;
		}

		MpNet_Fail("could not send a packet to the relay");
		s_net.connected = false;
		return false;
	}

	return true;
}

static bool MpTransport_Net_Poll(uint8 slot, uint32 turn, MpPacket *packet)
{
	uint32 i;

	MpNet_Pump();

	if (slot >= MATCH_SLOT_MAX) return false;

	i = turn % MP_NET_WINDOW;

	if (!s_net.window[slot][i].used || s_net.window[slot][i].turn != turn) return false;

	*packet = s_net.window[slot][i].packet;

	return true;
}

static const MpTransport s_transportNet = {
	&MpTransport_Net_Send,
	&MpTransport_Net_Poll
};

const MpTransport *MpTransport_Net(void)
{
	return &s_transportNet;
}
