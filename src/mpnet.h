/** @file src/mpnet.h The socket that carries turn packets to the relay. */

#ifndef MPNET_H
#define MPNET_H

#include "mpturn.h"

extern bool MpNet_Connect(const char *host, uint16 port, const char *room, uint8 slot);
extern void MpNet_Disconnect(void);
extern bool MpNet_IsConnected(void);
extern bool MpNet_IsReady(void);
extern bool MpNet_HasLeft(uint8 *slot);

/* Where the link stands, for a corner of the screen and for the harnesses. */
typedef enum MpLinkState {
	MP_LINK_UP,                                             /*!< Both players present, socket alive. */
	MP_LINK_RECONNECTING,                                   /*!< Our socket died; dialling the relay again. */
	MP_LINK_PEER_GONE,                                      /*!< The relay said LEFT; waiting for them to rejoin. */
	MP_LINK_DOWN                                            /*!< Recovery ran out, or never began.  The match is over. */
} MpLinkState;
extern MpLinkState MpNet_GetLinkState(uint32 *sinceMs, uint32 *relinks);
extern void MpNet_SetLogger(void (*log)(const char *line));
extern void MpNet_SetDropAt(uint32 turn);
extern const char *MpNet_GetError(void);

extern void MpNet_Pump(void);

extern const MpTransport *MpTransport_Net(void);

#endif /* MPNET_H */
