/** @file src/mpnet.c The socket that carries turn packets to the relay. */

#include <stdarg.h>
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
	/* The worst turn on the wire -- MP_TURN_COMMANDS_MAX commands each naming
	 * MP_COMMAND_UNITS_MAX recipients with three-digit indices -- is a little
	 * over 14 KB.  This was 8192, and MpPacket_Format() cut the tail off in
	 * silence: the receiver could not parse what was left, dropped the packet
	 * without a word, and the match froze on both sides for good. */
	MP_NET_PAYLOAD = 16384,

	/* Link recovery.  A dial every two seconds, for as long as a player can
	 * reasonably take to get a Wi-Fi or a VPN tunnel back.  A link that died
	 * without a FIN leaves a ghost of us in the slot, and a rejoin over it is
	 * refused with "slot is taken" -- but the relay forgets a silent client in
	 * 12 s (see the keepalive below), which is sooner than we notice the
	 * silence ourselves, so the seat is free by the time we ask for it. */
	MP_NET_REDIAL_MS  = 2000,
	MP_NET_RECOVER_MS = 150000,

	/* A client waiting for the other one sends nothing, and the relay drops a
	 * client it has not heard from in 12 s -- so the one who stayed would be
	 * thrown out for waiting.  Every 5 s of silence the newest own turn is sent
	 * again: the other side, if present, stores the same bytes over the same
	 * turn and nothing changes.  Before there is a turn to send, an empty frame
	 * ("PKT 0") goes instead, which the relay forwards and the other side
	 * ignores.  This is also what makes a dead link *noticeable*: with the
	 * other player alive, something arrives at least every 5 s. */
	MP_NET_KEEPALIVE_MS = 5000,

	/* A full room that says nothing for this long has a dead link in it, and
	 * TCP will not say so for minutes: a path that drops packets silently -- a
	 * NAT that forgot the mapping, a VPN tunnel that went down -- leaves the
	 * socket "connected" with every send disappearing into retransmission.
	 * Measured over a flapping tunnel, the client learned of it 14 to 59 s
	 * later.  Now it hangs up and dials again itself.  Longer than the relay's
	 * 12 s, so that when it is *our* path that died the relay has already
	 * told the other side and cleared our seat by the time we dial. */
	MP_NET_SILENCE_MS = 15000
};

static struct {
	bool connected;
	bool ready;
	bool left;
	uint8 leftSlot;
	uint8 localSlot;
	MpSocket socket;
	char error[160];

	/* What the last dial asked for, so the link can be made again. */
	char host[64];
	uint16 port;
	char room[80];

	/* A dial in flight, so a redial never blocks the frame that started it. */
	MpSocket pending;
	uint32 pendingSince;

	/* Recovery.  The socket is gone and we are dialling again (recovering), or
	 * the relay said LEFT and has not said READY since (peerGone).  Either way
	 * the turn loop stalls -- Send and Poll answer false -- and the match ends
	 * only when MP_NET_RECOVER_MS runs out.  MpNet_IsConnected() and
	 * MpNet_HasLeft() are answered from this, which is what turns "the match
	 * is over" into "the match is waiting" for every caller at once. */
	bool recovering;
	uint32 lostAt;
	uint32 nextDial;
	uint16 dials;
	bool peerGone;
	uint32 peerGoneAt;
	uint32 relinks;
	bool resending;
	uint32 lastSent;

	/* Silence detection: the last time anything at all came down the socket,
	 * and whether this connection has been answered by the relay yet. */
	uint32 lastRecv;
	bool welcomed;

	/* --mp-drop-at: cut the link ourselves when our packet for this turn goes
	 * out, so recovery can be tested without a cable to pull. */
	uint32 dropAt;

	void (*log)(const char *line);

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

static void MpNet_Log(const char *fmt, ...)
{
	char line[200];
	va_list ap;

	if (s_net.log == NULL) return;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	s_net.log(line);
}

void MpNet_SetLogger(void (*log)(const char *line))
{
	s_net.log = log;
}

void MpNet_SetDropAt(uint32 turn)
{
	s_net.dropAt = turn;
}

const char *MpNet_GetError(void)
{
	return s_net.error;
}

/* Connected, or still trying to be: a link in recovery is not a lost match. */
bool MpNet_IsConnected(void)
{
	return s_net.connected || s_net.recovering;
}

bool MpNet_IsReady(void)
{
	return s_net.ready;
}

/* Only once the wait for them is over.  While it runs, "left" is "dropped". */
bool MpNet_HasLeft(uint8 *slot)
{
	if (slot != NULL) *slot = s_net.leftSlot;
	if (!s_net.peerGone) return false;

	return Timer_GetTime() - s_net.peerGoneAt > MP_NET_RECOVER_MS;
}

MpLinkState MpNet_GetLinkState(uint32 *sinceMs, uint32 *relinks)
{
	const uint32 now = Timer_GetTime();

	if (relinks != NULL) *relinks = s_net.relinks;
	if (sinceMs != NULL) *sinceMs = 0;

	if (s_net.recovering) {
		if (sinceMs != NULL) *sinceMs = now - s_net.lostAt;
		return MP_LINK_RECONNECTING;
	}
	if (!s_net.connected) return MP_LINK_DOWN;
	if (s_net.peerGone) {
		if (sinceMs != NULL) *sinceMs = now - s_net.peerGoneAt;
		return MpNet_HasLeft(NULL) ? MP_LINK_DOWN : MP_LINK_PEER_GONE;
	}

	return MP_LINK_UP;
}

/* ---------------------------------------------------------------------------
 * Dialling.
 *
 * Split into a start and a poll so that a redial from inside the frame loop
 * never blocks it: the SYN goes out in one frame and the answer is collected in
 * a later one.  The first connect, before the match, wants to wait, and does so
 * by polling in a loop.
 * ------------------------------------------------------------------------- */

static void MpSocket_SetNonBlockingSock(MpSocket sock)
{
	MpSocket_SetNonBlocking(sock);
}

/** Resolve and send the SYN.  True if a connect is now in flight (or done). */
static bool MpNet_DialStart(void)
{
	struct addrinfo hints;
	struct addrinfo *results = NULL;
	struct addrinfo *entry;
	char service[16];
	MpSocket sock = MP_SOCKET_INVALID;

	if (s_net.pending != MP_SOCKET_INVALID) return true;

	snprintf(service, sizeof(service), "%u", (unsigned)s_net.port);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(s_net.host, service, &hints, &results) != 0 || results == NULL) {
		MpNet_Fail("could not resolve the relay address");
		return false;
	}

	for (entry = results; entry != NULL; entry = entry->ai_next) {
		sock = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
		if (sock == MP_SOCKET_INVALID) continue;

		MpSocket_SetNonBlockingSock(sock);

		if (connect(sock, entry->ai_addr, (socklen_t)entry->ai_addrlen) == 0) break;
		if (MpSocket_InProgress()) break;

		MpSocket_Close(sock);
		sock = MP_SOCKET_INVALID;
	}

	freeaddrinfo(results);

	if (sock == MP_SOCKET_INVALID) {
		MpNet_Fail("could not reach the relay");
		return false;
	}

	s_net.pending      = sock;
	s_net.pendingSince = Timer_GetTime();

	return true;
}

/**
 * Has the SYN been answered?  1 connected, 0 still waiting, -1 failed.
 *
 * select() is restarted on EINTR here, which is exactly what a blocking
 * connect() cannot be: the game arms a 60 Hz SIGALRM with no SA_RESTART
 * (Timer_InterruptResume() in timer.c), so every blocking system call in the
 * process is interrupted about every 16 ms.  A connect() to anything further
 * away than loopback therefore returned EINTR every single time, and was
 * reported as "could not reach the relay" -- a sentence about the server that
 * was never true, and one no test could see, because a relay on 127.0.0.1
 * connects before the first signal arrives.
 */
static int MpNet_DialPoll(void)
{
	struct timeval tv;
	fd_set writable;
	int ready;
	int err = 0;
#if defined(_WIN32)
	int errLen = sizeof(err);
#else
	socklen_t errLen = sizeof(err);
#endif /* _WIN32 */

	if (s_net.pending == MP_SOCKET_INVALID) return -1;

	if (Timer_GetTime() - s_net.pendingSince > MP_NET_CONNECT_MS) {
		MpSocket_Close(s_net.pending);
		s_net.pending = MP_SOCKET_INVALID;
		MpNet_Fail("could not reach the relay");
		return -1;
	}

	tv.tv_sec  = 0;
	tv.tv_usec = 0;
	FD_ZERO(&writable);
	FD_SET(s_net.pending, &writable);

	ready = select((int)s_net.pending + 1, NULL, &writable, NULL, &tv);
	if (ready == 0) return 0;
	if (ready < 0) return MpSocket_Interrupted() ? 0 : -1;

	if (getsockopt(s_net.pending, SOL_SOCKET, SO_ERROR, (char *)&err, &errLen) != 0 || err != 0) {
		MpSocket_Close(s_net.pending);
		s_net.pending = MP_SOCKET_INVALID;
		MpNet_Fail("could not reach the relay");
		return -1;
	}

	return 1;
}

/** The SYN was answered: claim the seat.  Leaves the socket non-blocking. */
static bool MpNet_DialFinish(void)
{
	char request[128];
	int one = 1;
	MpSocket sock = s_net.pending;

	s_net.pending = MP_SOCKET_INVALID;

	/* Every turn is one small packet the other player is already waiting for, so
	 * waiting to fill a segment is exactly the wrong trade. */
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

	snprintf(request, sizeof(request), "JOIN %s %u\n", s_net.room, (unsigned)s_net.localSlot);

	if (!MpSocket_SendAll(sock, request, (uint32)strlen(request))) {
		MpSocket_Close(sock);
		MpNet_Fail("could not send the join request");
		return false;
	}

	if (s_net.socket != MP_SOCKET_INVALID) MpSocket_Close(s_net.socket);

	s_net.socket    = sock;
	s_net.connected = true;
	s_net.ready     = false;
	s_net.welcomed  = false;
	s_net.inUsed    = 0;
	s_net.lastSent  = Timer_GetTime();
	s_net.lastRecv  = s_net.lastSent;

	return true;
}

/**
 * Dial the relay and claim a slot in a room, waiting for the answer.
 *
 * Three attempts, because a single refused connect is not evidence of
 * anything: transparent proxies and carrier middleboxes drop the odd outbound
 * SYN, and on this machine two clients dialling the same relay at the same
 * moment lose one often enough to be annoying.
 */
bool MpNet_Connect(const char *host, uint16 port, const char *room, uint8 slot)
{
	uint16 attempt;
	void (*log)(const char *) = s_net.log;
	uint32 dropAt = s_net.dropAt;

#if defined(_WIN32)
	WSADATA wsa;

	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		MpNet_Fail("could not start winsock");
		return false;
	}
#endif /* _WIN32 */

	memset(&s_net, 0, sizeof(s_net));
	s_net.socket    = MP_SOCKET_INVALID;
	s_net.pending   = MP_SOCKET_INVALID;
	s_net.localSlot = slot;
	s_net.port      = port;
	s_net.log       = log;
	s_net.dropAt    = dropAt;
	snprintf(s_net.host, sizeof(s_net.host), "%s", host);
	snprintf(s_net.room, sizeof(s_net.room), "%s", room);

	for (attempt = 0; attempt < 3; attempt++) {
		int state;

		if (attempt != 0) msleep(250);
		if (!MpNet_DialStart()) continue;

		while ((state = MpNet_DialPoll()) == 0) msleep(5);
		if (state < 0) continue;

		return MpNet_DialFinish();
	}

	return false;
}

/* The socket died under a running match.  Stall, and start dialling again. */
static void MpNet_LinkLost(const char *why)
{
	if (s_net.socket != MP_SOCKET_INVALID) MpSocket_Close(s_net.socket);
	s_net.socket = MP_SOCKET_INVALID;

	MpNet_Fail(why);

	/* An outage is measured from when the room last worked, not from the last
	 * hang-up: a relink that never got as far as READY -- a relay that takes
	 * the connection and says nothing -- is the same outage continuing, and
	 * restarting the clock on it would make the wait endless. */
	if (s_net.ready || s_net.lostAt == 0) s_net.lostAt = Timer_GetTime();

	s_net.connected  = false;
	s_net.recovering = true;
	s_net.nextDial   = Timer_GetTime();
	s_net.dials      = 0;
	s_net.inUsed     = 0;

	MpNet_Log("mp-net: link lost (%s), dialling again for up to %u s",
	          why, (unsigned)(MP_NET_RECOVER_MS / 1000));
}

/* Something that will not get better by dialling again. */
static void MpNet_LinkDead(const char *why)
{
	if (s_net.socket != MP_SOCKET_INVALID) MpSocket_Close(s_net.socket);
	s_net.socket = MP_SOCKET_INVALID;

	MpNet_Fail(why);
	s_net.connected  = false;
	s_net.recovering = false;

	MpNet_Log("mp-net: %s", why);
}

static bool MpNet_SendFrame(const MpPacket *packet, bool pumpWhileBlocked);
static void MpNet_Keepalive(void);

/**
 * Send every turn of ours the window still holds, oldest first.
 *
 * Called when the relay says READY, which it does each time the room is full
 * again -- so after either side has dropped and come back, both resend what the
 * other may have missed in flight.  A packet the other side already has is
 * stored again with the same content, which changes nothing; a match stalled
 * on a lost link never advanced, so the gap is at most `delay` turns and the
 * window is 64 deep.
 */
static void MpNet_ResendWindow(void)
{
	uint32 lo = 0xFFFFFFFF;
	uint32 hi = 0;
	uint32 turn;
	uint32 i;
	uint32 sent = 0;

	if (s_net.resending) return;

	for (i = 0; i < MP_NET_WINDOW; i++) {
		if (!s_net.window[s_net.localSlot][i].used) continue;
		if (s_net.window[s_net.localSlot][i].turn < lo) lo = s_net.window[s_net.localSlot][i].turn;
		if (s_net.window[s_net.localSlot][i].turn > hi) hi = s_net.window[s_net.localSlot][i].turn;
	}
	if (lo > hi) return;

	s_net.resending = true;
	for (turn = lo; turn <= hi && s_net.connected; turn++) {
		i = turn % MP_NET_WINDOW;
		if (!s_net.window[s_net.localSlot][i].used || s_net.window[s_net.localSlot][i].turn != turn) continue;
		if (!MpNet_SendFrame(&s_net.window[s_net.localSlot][i].packet, false)) break;
		sent++;
	}
	s_net.resending = false;

	MpNet_Log("mp-net: sent turns %u..%u again (%u packets)", (unsigned)lo, (unsigned)hi, (unsigned)sent);
}

static void MpNet_Recover(void)
{
	const uint32 now = Timer_GetTime();
	int state;

	if (now - s_net.lostAt > MP_NET_RECOVER_MS) {
		char why[160];

		if (s_net.pending != MP_SOCKET_INVALID) MpSocket_Close(s_net.pending);
		s_net.pending = MP_SOCKET_INVALID;

		snprintf(why, sizeof(why), "the relay could not be reached again in %u s (%u attempts)",
		         (unsigned)(MP_NET_RECOVER_MS / 1000), (unsigned)s_net.dials);
		MpNet_LinkDead(why);
		return;
	}

	if (s_net.pending == MP_SOCKET_INVALID) {
		if (now < s_net.nextDial) return;
		s_net.nextDial = now + MP_NET_REDIAL_MS;
		s_net.dials++;
		if (!MpNet_DialStart()) return;
	}

	state = MpNet_DialPoll();
	if (state == 0) return;
	if (state < 0) return;

	if (!MpNet_DialFinish()) return;

	s_net.recovering = false;
	s_net.relinks++;

	MpNet_Log("mp-net: relinked after %u ms and %u attempt%s",
	          (unsigned)(now - s_net.lostAt), (unsigned)s_net.dials, (s_net.dials == 1) ? "" : "s");
}

void MpNet_Disconnect(void)
{
	if (s_net.socket != MP_SOCKET_INVALID) MpSocket_Close(s_net.socket);
	if (s_net.pending != MP_SOCKET_INVALID) MpSocket_Close(s_net.pending);

	s_net.socket     = MP_SOCKET_INVALID;
	s_net.pending    = MP_SOCKET_INVALID;
	s_net.connected  = false;
	s_net.recovering = false;

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
				MpNet_LinkDead("the relay sent a malformed packet header");
				return consumed + headerLength;
			}

			if (length > MP_NET_PAYLOAD) {
				MpNet_LinkDead("the relay sent an oversized packet");
				return consumed + headerLength;
			}

			/* The body has not all arrived yet: leave the whole message in the
			 * buffer and try again after the next read. */
			if (left < headerLength + length) break;

			/* An empty frame is the other player's keepalive from before their
			 * first turn: it fed the relay's idle clock, and it says they are
			 * there.  There is nothing in it to store. */
			if (length == 0) {
				consumed += headerLength;
				continue;
			}

			memcpy(body, start + headerLength, length);
			body[length] = '\0';

			/* A packet this build cannot read is the end of the match, said
			 * out loud.  It used to be dropped in silence, which left the turn
			 * loop waiting for it for ever -- a frozen screen on both sides
			 * with nothing on either of them to say why. */
			if (MpPacket_Parse(body, &packet)) {
				MpNet_Store((uint8)slot, &packet);
			} else {
				MpNet_LinkDead("the other player sent a turn this build cannot read");
				return consumed + headerLength + length;
			}

			consumed += headerLength + length;
			continue;
		}

		/* The relay's own lines are rare and short, and when a match breaks
		 * they are the evidence: keep every one of them. */
		MpNet_Log("mp-net: relay: %.*s", (int)(headerLength - 1), start);

		if (strncmp(start, "READY", 5) == 0) {
			s_net.ready = true;

			/* The room is full again.  Whether it was us or them who dropped,
			 * whatever was in flight at that moment is gone: send ours again. */
			if (s_net.peerGone) {
				MpNet_Log("mp-net: they are back after %u ms", (unsigned)(Timer_GetTime() - s_net.peerGoneAt));
				s_net.peerGone = false;
				s_net.left     = false;
			}
			consumed += headerLength;
			MpNet_ResendWindow();
			continue;
		} else if (strncmp(start, "LEFT ", 5) == 0) {
			/* A LEFT naming our own seat is the relay reporting our previous
			 * incarnation: after a redial the old connection's farewell can
			 * reach the new one, the room being full again by then.  Taken at
			 * face value it would have us waiting 150 s for an opponent who
			 * is right there, and then ending the match. */
			if (sscanf(start, "LEFT %u", &slot) == 1 && slot == s_net.localSlot) {
				MpNet_Log("mp-net: the relay said our old seat was left, which it was");
			} else if (sscanf(start, "LEFT %u", &slot) == 1) {
				s_net.left     = true;
				s_net.leftSlot = (uint8)slot;
				if (!s_net.peerGone) {
					s_net.peerGone   = true;
					s_net.peerGoneAt = Timer_GetTime();
					MpNet_Log("mp-net: the other player dropped, waiting up to %u s for them",
					          (unsigned)(MP_NET_RECOVER_MS / 1000));
				}
			}
		} else if (strncmp(start, "ERROR ", 6) == 0) {
			snprintf(s_net.error, sizeof(s_net.error), "%.*s", (int)(headerLength - 7), start + 6);
			MpNet_Log("mp-net: relay says: %s", s_net.error);
		} else if (strncmp(start, "WELCOME", 7) == 0) {
			/* The slot is what we asked for and the count only interests a
			 * lobby; what matters is that the relay answered at all. */
			s_net.welcomed = true;
		}

		consumed += headerLength;
	}

	return consumed;
}

/**
 * Drain the socket.
 *
 * Called from both halves of the transport, so a client that is only sending
 * still notices packets, a disconnection and the relay's own messages.  While
 * the link is being recovered this is also what drives the redial.
 */
void MpNet_Pump(void)
{
	if (!s_net.connected) {
		if (s_net.recovering) MpNet_Recover();
		if (!s_net.connected) return;
	}

	while (true) {
		uint32 consumed;
		int got;

		if (s_net.inUsed < sizeof(s_net.in)) {
			got = (int)recv(s_net.socket, s_net.in + s_net.inUsed, (int)(sizeof(s_net.in) - s_net.inUsed), 0);

			if (got > 0) {
				s_net.inUsed  += (uint32)got;
				s_net.lastRecv = Timer_GetTime();
			} else if (got == 0) {
				MpNet_LinkLost("the relay closed the connection");
				return;
			} else if (!MpSocket_WouldBlock() && !MpSocket_Interrupted()) {
				MpNet_LinkLost("the connection to the relay broke");
				return;
			}
		} else {
			got = 0;
		}

		if (!s_net.connected) return;

		consumed = MpNet_ParseBuffer();

		if (consumed != 0 && s_net.connected) {
			memmove(s_net.in, s_net.in + consumed, s_net.inUsed - consumed);
			s_net.inUsed -= consumed;
		}

		/* Nothing new to read and nothing left to parse. */
		if (got <= 0 && consumed == 0) break;
		if (!s_net.connected) break;
	}

	if (s_net.connected) {
		const uint32 now = Timer_GetTime();

		if (now - s_net.lastSent > MP_NET_KEEPALIVE_MS) MpNet_Keepalive();

		/* Silence where there should be none.  With the room full and the other
		 * player present their keepalive arrives every 5 s, so 15 s of nothing
		 * means the link is dead whatever the socket says.  A room that is not
		 * full yet is allowed its silence -- that is the lobby waiting for the
		 * second player -- but a relay that has not even said WELCOME is not. */
		if (s_net.connected && s_net.ready && !s_net.peerGone && now - s_net.lastRecv > MP_NET_SILENCE_MS) {
			MpNet_LinkLost("nothing came from the relay for 15 s");
		} else if (s_net.connected && !s_net.welcomed && now - s_net.lastRecv > MP_NET_SILENCE_MS) {
			MpNet_LinkLost("the relay never answered the join");
		}
	}
}

/* -------------------------------------------------------------------------
 * The transport.
 * ------------------------------------------------------------------------- */

static bool MpNet_SendFrame(const MpPacket *packet, bool pumpWhileBlocked)
{
	char body[MP_NET_PAYLOAD];
	char frame[MP_NET_PAYLOAD + 64];
	uint16 used;
	uint32 total;
	uint32 sent = 0;

	used = MpPacket_Format(body, sizeof(body), packet);
	if (used == 0) {
		MpNet_LinkDead("a turn did not fit on the wire");
		return false;
	}

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
			if (pumpWhileBlocked) MpNet_Pump(); else msleep(1);
			if (!s_net.connected) return false;
			continue;
		}

		MpNet_LinkLost("could not send a packet to the relay");
		return false;
	}

	s_net.lastSent = Timer_GetTime();

	return true;
}

/* Our newest turn, sent again, so the relay hears from us while we wait. */
static void MpNet_Keepalive(void)
{
	uint32 hi = 0;
	uint32 i;
	const MpPacket *newest = NULL;

	for (i = 0; i < MP_NET_WINDOW; i++) {
		if (!s_net.window[s_net.localSlot][i].used) continue;
		if (newest == NULL || s_net.window[s_net.localSlot][i].turn >= hi) {
			hi     = s_net.window[s_net.localSlot][i].turn;
			newest = &s_net.window[s_net.localSlot][i].packet;
		}
	}

	if (newest == NULL) {
		/* Nothing to say yet -- the lobby, before the first turn.  An empty
		 * frame keeps the relay's idle clock from running out on a player who
		 * is only waiting for the other one. */
		if (!MpSocket_SendAll(s_net.socket, "PKT 0\n", 6)) {
			MpNet_LinkLost("could not send a keepalive");
			return;
		}
		s_net.lastSent = Timer_GetTime();
		return;
	}

	MpNet_SendFrame(newest, false);
}

static bool MpTransport_Net_Send(uint8 slot, const MpPacket *packet)
{
	MpNet_Pump();

	if (!s_net.connected) return false;

	/* Our own packets go straight into the window rather than round trip through
	 * the relay: we are one of the players, and waiting to hear our own move
	 * back would put a whole ping into every turn for no reason. */
	MpNet_Store(slot, packet);

	if (slot != s_net.localSlot) return true;

	if (s_net.dropAt != 0 && packet->turn >= s_net.dropAt) {
		s_net.dropAt = 0;
		MpNet_LinkLost("--mp-drop-at cut the link");
		return false;
	}

	return MpNet_SendFrame(packet, true);
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
