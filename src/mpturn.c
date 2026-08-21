/** @file src/mpturn.c The lockstep turn loop: when a command happens. */

#include <stdio.h>
#include <string.h>
#include "types.h"
#include "os/common.h"

#include "mpturn.h"

#include "house.h"
#include "match.h"
#include "mpcommand.h"
#include "mpsync.h"
#include "timer.h"

static struct {
	bool active;
	uint8 localSlot;
	uint16 turnLength;
	uint8 delay;
	const MpTransport *transport;

	uint32 turn;                                            /*!< The turn about to be simulated. */
	uint32 nextBoundary;                                    /*!< Game tick at which this turn ends. */
	uint32 stalls;                                          /*!< Ticks spent waiting for somebody's packet. */
	bool desynced;
	uint32 desyncTurn;

	MpPacket outbox;                                        /*!< What we have collected since the last send. */
} s_turn;

/**
 * Start playing in turns.
 *
 * @param localSlot Which match slot this process is.
 * @param transport How packets reach the others.
 * @param turnLength Ticks per turn.
 * @param delay Turns between issuing a command and running it.  This is the
 *   whole latency budget: a packet has delay * turnLength ticks to arrive, and
 *   the game never waits if it does.
 */
void MpTurn_Begin(uint8 localSlot, const MpTransport *transport, uint16 turnLength, uint8 delay)
{
	uint8 i;

	memset(&s_turn, 0, sizeof(s_turn));

	if (transport == NULL || turnLength == 0 || delay == 0) return;

	s_turn.active       = true;
	s_turn.localSlot    = localSlot;
	s_turn.transport    = transport;
	s_turn.turnLength   = turnLength;
	s_turn.delay        = delay;
	s_turn.turn         = 0;
	/* Turn T runs during ticks [T*TL, (T+1)*TL), and its commands are applied at
	 * the start of it -- so turn 0 is due on tick 0, before anything simulates. */
	s_turn.nextBoundary = 0;

	/* The first `delay` turns have to be filled in before anybody has had a
	 * chance to issue anything, or the match stalls on tick one waiting for
	 * packets nobody could have sent. */
	for (i = 0; i < delay; i++) {
		MpPacket empty;

		memset(&empty, 0, sizeof(empty));
		empty.turn      = i;
		empty.checkTurn = MP_TURN_NO_CHECKSUM;
		transport->send(localSlot, &empty);
	}

	s_turn.outbox.turn = delay;
}

void MpTurn_End(void)
{
	memset(&s_turn, 0, sizeof(s_turn));
}

bool MpTurn_IsActive(void)
{
	return s_turn.active;
}

uint32 MpTurn_GetTurn(void)
{
	return s_turn.turn;
}

uint32 MpTurn_GetStalls(void)
{
	return s_turn.stalls;
}

bool MpTurn_HasDesynced(uint32 *turn)
{
	if (turn != NULL) *turn = s_turn.desyncTurn;

	return s_turn.desynced;
}

/**
 * Take a command from the player.
 *
 * It does not happen now.  It goes into the packet for a turn far enough ahead
 * that everybody's copy will have arrived by the time it runs -- which is the
 * one thing that makes a command deterministic on two machines, and the reason
 * the local player sees their own order take effect a fraction of a second
 * late.  See mp.md.
 */
bool MpTurn_Submit(const MpCommand *cmd)
{
	if (!s_turn.active || cmd == NULL) return false;
	if (s_turn.outbox.count >= MP_TURN_COMMANDS_MAX) return false;

	s_turn.outbox.cmd[s_turn.outbox.count++] = *cmd;

	return true;
}

/** Whether the clock has reached the end of the current turn. */
bool MpTurn_IsDue(void)
{
	return (s_turn.active && g_timerGame >= s_turn.nextBoundary);
}

/**
 * Close one turn and open the next.
 *
 * Returns false when somebody's packet has not arrived: the caller must then
 * hold the simulation clock still and try again.  That stall is the only way
 * latency is ever allowed to show, and it is why the turn delay exists.
 */
bool MpTurn_Advance(void)
{
	MpPacket packet;
	uint32 checkTurn;
	uint32 checksum = 0;
	bool haveChecksum = false;
	uint8 slot;

	if (!s_turn.active) return true;

	/* Ours first, so a single-player-in-two-processes match cannot deadlock on
	 * itself, and so the others have the longest possible time to receive it. */
	if (s_turn.outbox.turn == s_turn.turn + s_turn.delay) {
		MpSyncChecksum state;

		s_turn.outbox.checkTurn = s_turn.turn;
		s_turn.outbox.checksum  = MpSync_Take(&state) ? state.total : 0;

		if (!s_turn.transport->send(s_turn.localSlot, &s_turn.outbox)) return false;

		memset(&s_turn.outbox, 0, sizeof(s_turn.outbox));
		s_turn.outbox.turn = s_turn.turn + s_turn.delay + 1;
	}

	/* Everybody's packet for this turn, or nobody moves. */
	for (slot = 0; slot < MATCH_SLOT_MAX; slot++) {
		if (Match_GetSlotHouse(slot) == HOUSE_INVALID) continue;

		if (!s_turn.transport->poll(slot, s_turn.turn, &packet)) {
			s_turn.stalls++;
			return false;
		}
	}

	/* Apply in slot order, never in arrival order: two clients that received the
	 * same packets in a different order must still run them in the same one. */
	for (slot = 0; slot < MATCH_SLOT_MAX; slot++) {
		uint16 i;

		if (Match_GetSlotHouse(slot) == HOUSE_INVALID) continue;
		if (!s_turn.transport->poll(slot, s_turn.turn, &packet)) continue;

		/* Every packet carries a checksum of the same past turn, so they are
		 * comparable without anyone being the authority.  A mismatch is reported
		 * and the match stops being trustworthy from that moment; nobody's copy
		 * wins, because the loser's game would be corrupt either way. */
		checkTurn = packet.checkTurn;
		if (checkTurn != MP_TURN_NO_CHECKSUM) {
			if (!haveChecksum) {
				checksum = packet.checksum;
				haveChecksum = true;
			} else if (packet.checksum != checksum && !s_turn.desynced) {
				s_turn.desynced   = true;
				s_turn.desyncTurn = checkTurn;
			}
		}

		for (i = 0; i < packet.count && i < MP_TURN_COMMANDS_MAX; i++) {
			MpCommand_Execute(&packet.cmd[i]);
		}
	}

	s_turn.turn++;
	s_turn.nextBoundary += s_turn.turnLength;

	return true;
}

/* -------------------------------------------------------------------------
 * The wire format.
 *
 * Text, for the same reason the command recording is text: when two clients
 * disagree, the packets are the evidence, and a line per packet diffs where a
 * binary blob only says "different".  A turn's worth is a few dozen bytes.
 * ------------------------------------------------------------------------- */

uint16 MpPacket_Format(char *dst, uint16 size, const MpPacket *packet)
{
	uint16 used;
	uint16 i;

	used = (uint16)snprintf(dst, size, "turn %u check %u %08x count %u\n",
	                        (unsigned)packet->turn, (unsigned)packet->checkTurn,
	                        (unsigned)packet->checksum, (unsigned)packet->count);

	for (i = 0; i < packet->count && i < MP_TURN_COMMANDS_MAX; i++) {
		const MpCommand *cmd = &packet->cmd[i];
		uint16 u;

		if (used >= size) break;

		used += (uint16)snprintf(dst + used, size - used, "cmd %u %u %u %u %u %u %u",
		                         (unsigned)cmd->type, (unsigned)cmd->houseID, (unsigned)cmd->action,
		                         (unsigned)cmd->packed, (unsigned)cmd->object, (unsigned)cmd->value,
		                         (unsigned)cmd->count);

		for (u = 0; u < cmd->count && u < MP_COMMAND_UNITS_MAX && used < size; u++) {
			used += (uint16)snprintf(dst + used, size - used, " %u", (unsigned)cmd->unit[u]);
		}

		if (used < size) used += (uint16)snprintf(dst + used, size - used, "\n");
	}

	return used;
}

bool MpPacket_Parse(const char *src, MpPacket *packet)
{
	unsigned turn, checkTurn, checksum, count;
	const char *p = src;
	int consumed = 0;
	uint16 i;

	memset(packet, 0, sizeof(*packet));

	if (sscanf(p, "turn %u check %u %x count %u%n", &turn, &checkTurn, &checksum, &count, &consumed) != 4) return false;
	if (count > MP_TURN_COMMANDS_MAX) return false;

	packet->turn      = (uint32)turn;
	packet->checkTurn = (uint32)checkTurn;
	packet->checksum  = (uint32)checksum;
	packet->count     = (uint16)count;

	p += consumed;

	for (i = 0; i < packet->count; i++) {
		unsigned type, house, action, packed, object, value, num;
		MpCommand *cmd = &packet->cmd[i];
		uint16 u;

		consumed = 0;
		if (sscanf(p, " cmd %u %u %u %u %u %u %u%n", &type, &house, &action, &packed, &object, &value, &num, &consumed) != 7) return false;
		if (num > MP_COMMAND_UNITS_MAX) return false;

		p += consumed;

		cmd->type    = (uint8)type;
		cmd->houseID = (uint8)house;
		cmd->action  = (uint8)action;
		cmd->packed  = (uint16)packed;
		cmd->object  = (uint16)object;
		cmd->value   = (uint16)value;
		cmd->count   = (uint8)num;

		for (u = 0; u < cmd->count; u++) {
			unsigned index;

			consumed = 0;
			if (sscanf(p, " %u%n", &index, &consumed) != 1) return false;
			p += consumed;
			cmd->unit[u] = (uint16)index;
		}
	}

	return true;
}

/* -------------------------------------------------------------------------
 * Loopback: both slots in one process.
 *
 * Not a stand-in for the network -- it is the transport a local match uses, and
 * it is what lets the turn loop be tested without one.
 * ------------------------------------------------------------------------- */

/* A window, not a log: only the turns between the one being applied and the one
 * being filled are ever asked for, and that is delay + 2 of them.  A packet is
 * a couple of kilobytes, so keeping every turn of a long match would cost tens
 * of megabytes to hold what nobody reads twice. */
enum { MP_LOOPBACK_WINDOW = 32 };

static struct {
	bool used;
	uint32 turn;
	MpPacket packet;
} s_loopback[MATCH_SLOT_MAX][MP_LOOPBACK_WINDOW];

static bool MpTransport_Loopback_Send(uint8 slot, const MpPacket *packet)
{
	uint32 i;

	if (slot >= MATCH_SLOT_MAX) return false;

	i = packet->turn % MP_LOOPBACK_WINDOW;

	s_loopback[slot][i].packet = *packet;
	s_loopback[slot][i].turn   = packet->turn;
	s_loopback[slot][i].used   = true;

	return true;
}

static bool MpTransport_Loopback_Poll(uint8 slot, uint32 turn, MpPacket *packet)
{
	uint32 i;

	if (slot >= MATCH_SLOT_MAX) return false;

	i = turn % MP_LOOPBACK_WINDOW;

	if (!s_loopback[slot][i].used || s_loopback[slot][i].turn != turn) return false;

	*packet = s_loopback[slot][i].packet;

	return true;
}

static const MpTransport s_transportLoopback = {
	&MpTransport_Loopback_Send,
	&MpTransport_Loopback_Poll
};

void MpTransport_Loopback_Reset(void)
{
	memset(s_loopback, 0, sizeof(s_loopback));
}

const MpTransport *MpTransport_Loopback(void)
{
	return &s_transportLoopback;
}

/* -------------------------------------------------------------------------
 * File transport: two processes, one directory.
 *
 * A stepping stone with real value of its own: each process runs its own whole
 * simulation and sees the other only through packets, which is exactly the
 * shape a socket has.  What it does not have is loss, reordering or a relay --
 * so it proves the turn loop, not the network.
 * ------------------------------------------------------------------------- */

static char s_fileDirectory[512];

/* Emulated one-way delivery time.  A packet is on disk the instant it is
 * written, which is a wire no real player has; holding it back is how the ping
 * table in mp.md gets measured rather than argued about.
 *
 * The delay is on the *sending* side, and that detail is the whole point: a
 * first attempt held the packet back from whoever read it, measured from the
 * moment they first looked -- so a packet that had been sitting there for two
 * turns still cost a full lag when somebody finally asked for it, and raising
 * the turn delay changed nothing.  Latency is measured from the send. */
static uint32 s_fileLagMs = 0;

enum { MP_FILE_PENDING_MAX = 64 };

static struct {
	bool used;
	uint32 dueAt;
	uint8 slot;
	MpPacket packet;
} s_filePending[MP_FILE_PENDING_MAX];

void MpTransport_File_SetLag(uint32 milliseconds)
{
	s_fileLagMs = milliseconds;
	memset(s_filePending, 0, sizeof(s_filePending));
}

static void MpTransport_File_Name(char *dst, uint16 size, uint8 slot, uint32 turn)
{
	snprintf(dst, size, "%s/s%u-t%u.pkt", s_fileDirectory, (unsigned)slot, (unsigned)turn);
}

static bool MpTransport_File_Write(uint8 slot, const MpPacket *packet)
{
	char temporary[512];
	char final[512];
	char body[8192];
	FILE *fp;
	uint16 used;

	if (s_fileDirectory[0] == '\0') return false;

	used = MpPacket_Format(body, sizeof(body), packet);

	/* Written beside the real name and renamed into place: the reader is another
	 * process polling the directory, and a half-written packet parses as a short
	 * one rather than as an absent one. */
	snprintf(temporary, sizeof(temporary), "%s/s%u-t%u.tmp", s_fileDirectory, (unsigned)slot, (unsigned)packet->turn);
	MpTransport_File_Name(final, sizeof(final), slot, packet->turn);

	fp = fopen(temporary, "w");
	if (fp == NULL) return false;

	if (fwrite(body, 1, used, fp) != used) {
		fclose(fp);
		return false;
	}

	fclose(fp);

	return (rename(temporary, final) == 0);
}

/** Put on the wire everything whose delivery time has come. */
static void MpTransport_File_Flush(void)
{
	uint32 now = Timer_GetTime();
	uint16 i;

	for (i = 0; i < MP_FILE_PENDING_MAX; i++) {
		if (!s_filePending[i].used) continue;
		if (s_filePending[i].dueAt > now) continue;

		MpTransport_File_Write(s_filePending[i].slot, &s_filePending[i].packet);
		s_filePending[i].used = false;
	}
}

static bool MpTransport_File_Send(uint8 slot, const MpPacket *packet)
{
	uint16 i;

	if (s_fileLagMs == 0) return MpTransport_File_Write(slot, packet);

	MpTransport_File_Flush();

	for (i = 0; i < MP_FILE_PENDING_MAX; i++) {
		if (s_filePending[i].used) continue;

		s_filePending[i].used   = true;
		s_filePending[i].dueAt  = Timer_GetTime() + s_fileLagMs;
		s_filePending[i].slot   = slot;
		s_filePending[i].packet = *packet;

		return true;
	}

	/* The queue only fills if the lag is longer than the whole match; sending
	 * straight through beats dropping the packet and deadlocking everybody. */
	return MpTransport_File_Write(slot, packet);
}

static bool MpTransport_File_Poll(uint8 slot, uint32 turn, MpPacket *packet)
{
	char name[512];
	char body[8192];
	size_t got;
	FILE *fp;

	if (s_fileDirectory[0] == '\0') return false;

	MpTransport_File_Flush();

	MpTransport_File_Name(name, sizeof(name), slot, turn);

	fp = fopen(name, "r");
	if (fp == NULL) return false;

	got = fread(body, 1, sizeof(body) - 1, fp);
	fclose(fp);

	body[got] = '\0';

	return MpPacket_Parse(body, packet);
}

static const MpTransport s_transportFile = {
	&MpTransport_File_Send,
	&MpTransport_File_Poll
};

const MpTransport *MpTransport_File(const char *directory)
{
	snprintf(s_fileDirectory, sizeof(s_fileDirectory), "%s", directory);

	return &s_transportFile;
}
