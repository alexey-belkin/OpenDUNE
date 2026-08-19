/** @file src/mpsync.h Simulation state checksum, for desync hunting. */

#ifndef MPSYNC_H
#define MPSYNC_H

/**
 * A checksum of the whole simulation state, split by savegame chunk so a
 * mismatch says *what* diverged and not merely *that* something did.
 */
typedef struct MpSyncChecksum {
	uint32 info;                                            /*!< Scenario and global game state. */
	uint32 house;                                           /*!< The House pool. */
	uint32 unit;                                            /*!< The Unit pool. */
	uint32 structure;                                       /*!< The Structure pool. */
	uint32 map;                                             /*!< The 64x64 tile map. */
	uint32 team;                                            /*!< The Team pool. */
	uint32 unitNew;                                         /*!< Unit fields this fork added. */
	uint32 rng;                                             /*!< Both random generators. */
	uint32 total;                                           /*!< All of the above, combined. */
} MpSyncChecksum;

extern uint32 MpSync_Crc32(uint32 crc, const void *buf, uint32 length);
extern bool MpSync_Take(MpSyncChecksum *checksum);
extern void MpSync_Format(char *dst, uint16 size, uint32 tick, const MpSyncChecksum *checksum);

#endif /* MPSYNC_H */
