/** @file src/mpturn.h The lockstep turn loop: when a command happens. */

#ifndef MPTURN_H
#define MPTURN_H

#include "match.h"
#include "mpcommand.h"
#include "mpsync.h"

enum {
	MP_TURN_LENGTH_DEFAULT = 8,                             /*!< Ticks per turn: 8 at 60 Hz is 133 ms. */
	MP_TURN_DELAY_DEFAULT  = 3,                             /*!< A command issued during turn N runs at turn N+D. */
	MP_TURN_COMMANDS_MAX   = 32                             /*!< Commands one player may issue in one turn. */
};

/** A packet describing no state: the opening turns, before there is any. */
#define MP_TURN_NO_CHECKSUM 0xFFFFFFFF

/**
 * What every client sends every turn, empty or not.
 *
 * An empty packet is the "I am here and I did nothing" acknowledgement, and it
 * is what keeps the other side simulating -- a client that sends nothing stalls
 * everybody, which is the whole bargain of lockstep.
 */
typedef struct MpPacket {
	uint32 turn;                                            /*!< The turn these commands belong to. */
	uint32 checkTurn;                                       /*!< Which turn the checksum below describes. */
	MpSyncChecksum check;                                   /*!< State as of checkTurn, chunk by chunk. */
	uint16 count;                                           /*!< Commands in cmd[]. */
	MpCommand cmd[MP_TURN_COMMANDS_MAX];
} MpPacket;

/**
 * How packets reach the other players.
 *
 * The turn loop knows nothing else about the network.  A loopback carries them
 * across a slot in the same process, a file transport across two processes on
 * one disk, and a socket across the internet -- the loop above cannot tell.
 */
typedef struct MpTransport {
	bool (*send)(uint8 slot, const MpPacket *packet);       /*!< Hand our packet to the others. */
	bool (*poll)(uint8 slot, uint32 turn, MpPacket *packet);/*!< Fetch a slot's packet for a turn, false if it has not arrived. */
} MpTransport;

extern void MpTurn_Begin(uint8 localSlot, const MpTransport *transport, uint16 turnLength, uint8 delay);
extern void MpTurn_End(void);
extern bool MpTurn_IsActive(void);
extern void MpTurn_SetDelay(uint8 delay);
extern uint8 MpTurn_GetDelay(void);

extern bool MpTurn_Submit(const MpCommand *cmd);
extern bool MpTurn_Advance(void);
extern bool MpTurn_IsDue(void);

extern uint32 MpTurn_GetTurn(void);
extern uint32 MpTurn_GetStalls(void);
extern bool MpTurn_HasDesynced(uint32 *turn);
extern const char *MpTurn_GetDesyncChunks(void);
extern void MpTurn_SetSnapshots(bool enabled);

extern uint16 MpPacket_Format(char *dst, uint16 size, const MpPacket *packet);
extern bool MpPacket_Parse(const char *src, MpPacket *packet);

extern const MpTransport *MpTransport_Loopback(void);
extern void MpTransport_Loopback_Reset(void);

extern const MpTransport *MpTransport_File(const char *directory);
extern void MpTransport_File_SetLag(uint32 milliseconds);

#endif /* MPTURN_H */
