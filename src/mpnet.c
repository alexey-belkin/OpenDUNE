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
	#define MpSocket_Interrupted() (WSAGetLastError() == WSAEINTR)
	#define MpSocket_InProgress() (WSAGetLastError() == WSAEWOULDBLOCK)
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
	#include <sys/select.h>
	#include <sys/time.h>
	#define MpSocket_WouldBlock() (errno == EAGAIN || errno == EWOULDBLOCK)
	#define MpSocket_Interrupted() (errno == EINTR)
	#define MpSocket_InProgress() (errno == EINPROGRESS)
#endif /* _WIN32 */

#include "types.h"
#include "os/common.h"
#include "os/sleep.h"

#include "mpnet.h"

#include "timer.h"

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

/* Long enough for a relay on the other side of the country, short enough that a
 * player who typed the address wrong is told so rather than left watching. */
#define MP_NET_CONNECT_MS 5000

static void MpSocket_SetNonBlocking(MpSocket sock)
{
#if defined(_WIN32)
	u_long mode = 1;

	ioctlsocket(sock, FIONBIO, &mode);
#else
	fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif /* _WIN32 */
}

/**
 * Dial one address, waiting up to MP_NET_CONNECT_MS for the handshake.
 *
 * **This may not be a blocking connect(), and that is the whole of it.** The
 * game arms a 60 Hz SIGALRM -- Timer_InterruptResume() in timer.c, with
 * sa_flags 0, so no SA_RESTART -- which means every blocking system call in the
 * process is interrupted about every 16 ms and nothing restarts it for us. A
 * connect() to anything further away than loopback takes longer than that, so
 * it returned EINTR every single time, and the three retries above could not
 * help: an interrupted connect carries on in the background, and a fresh socket
 * is interrupted just the same. Reported to the player as "could not reach the
 * relay", which is a sentence about the server and was never true.
 *
 * It is also why no test could see it. A relay on 127.0.0.1 connects before the
 * first signal arrives, and that is what --lobby-play and tools/mpduel.sh have
 * always been pointed at. Only a real relay 57 ms away fails.
 *
 * select() is restarted here on EINTR, which is exactly what connect() cannot
 * be. The socket is left non-blocking afterwards, the way the rest of the
 * module wants it.
 */
static bool MpSocket_ConnectWait(MpSocket sock, const struct sockaddr *addr, uint32 addrLen)
{
	const uint32 until = Timer_GetTime() + MP_NET_CONNECT_MS;
	int err = 0;

	MpSocket_SetNonBlocking(sock);

	if (connect(sock, addr, (socklen_t)addrLen) == 0) return true;
	if (!MpSocket_InProgress()) return false;

	while (true) {
		const uint32 now = Timer_GetTime();
		struct timeval tv;
		fd_set writable;
		int ready;

		if (now >= until) return false;

		tv.tv_sec  = (long)((until - now) / 1000);
		tv.tv_usec = (long)(((until - now) % 1000) * 1000);

		FD_ZERO(&writable);
		FD_SET(sock, &writable);

		ready = select((int)sock + 1, NULL, &writable, NULL, &tv);

		if (ready > 0) break;
		if (ready == 0) return false;
		if (!MpSocket_Interrupted()) return false;

		/* The 60 Hz timer, not the network.  Wait out what is left of it. */
	}

	/* A socket that became writable may still have failed: the error is
	 * collected here rather than reported by connect(). */
	{
#if defined(_WIN32)
		int errLen = sizeof(err);
#else
		socklen_t errLen = sizeof(err);
#endif /* _WIN32 */

		if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&err, &errLen) != 0) return false;
	}

	return err == 0;
}

/**
 * Put a whole request on the socket, which is already non-blocking.
 */
static bool MpSocket_SendAll(MpSocket sock, const char *data, uint32 total)
{
	uint32 sent = 0;

	while (sent < total) {
		const int wrote = (int)send(sock, data + sent, (int)(total - sent), 0);

		if (wrote > 0) {
			sent += (uint32)wrote;
			continue;
		}

		if (wrote < 0 && (MpSocket_WouldBlock() || MpSocket_Interrupted())) {
			msleep(1);
			continue;
		}

		return false;
	}

	return true;
}

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

			if (MpSocket_ConnectWait(sock, entry->ai_addr, (uint32)entry->ai_addrlen)) break;

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

	if (!MpSocket_SendAll(sock, request, (uint32)strlen(request))) {
		MpSocket_Close(sock);
		MpNet_Fail("could not send the join request");
		return false;
	}

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
			} else if (!MpSocket_WouldBlock() && !MpSocket_Interrupted()) {
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

		if (wrote < 0 && (MpSocket_WouldBlock() || MpSocket_Interrupted())) {
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
