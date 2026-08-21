/** @file src/mpnet.h The socket that carries turn packets to the relay. */

#ifndef MPNET_H
#define MPNET_H

#include "mpturn.h"

extern bool MpNet_Connect(const char *host, uint16 port, const char *room, uint8 slot);
extern void MpNet_Disconnect(void);
extern bool MpNet_IsConnected(void);
extern bool MpNet_IsReady(void);
extern bool MpNet_HasLeft(uint8 *slot);
extern const char *MpNet_GetError(void);

extern void MpNet_Pump(void);

extern const MpTransport *MpTransport_Net(void);

#endif /* MPNET_H */
