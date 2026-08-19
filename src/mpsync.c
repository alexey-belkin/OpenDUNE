/** @file src/mpsync.c Simulation state checksum, for desync hunting. */

#include <stdio.h>
#include <string.h>
#include "types.h"

#include "mpsync.h"

#include "saveload/saveload.h"
#include "structure.h"
#include "team.h"
#include "tools.h"

static uint32 s_crcTable[256];
static bool s_crcTableBuilt = false;

static void MpSync_BuildTable(void)
{
	uint32 i;

	for (i = 0; i < 256; i++) {
		uint32 c = i;
		uint16 bit;

		for (bit = 0; bit < 8; bit++) {
			c = ((c & 1) != 0) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
		}

		s_crcTable[i] = c;
	}

	s_crcTableBuilt = true;
}

/**
 * Continue a CRC-32 over another block of bytes.  Start with a crc of zero.
 */
uint32 MpSync_Crc32(uint32 crc, const void *buf, uint32 length)
{
	const uint8 *p = (const uint8 *)buf;
	uint32 c;

	if (!s_crcTableBuilt) MpSync_BuildTable();

	c = crc ^ 0xFFFFFFFF;
	while (length-- != 0) {
		c = s_crcTable[(c ^ *p++) & 0xFF] ^ (c >> 8);
	}

	return c ^ 0xFFFFFFFF;
}

/**
 * Run one savegame chunk writer into a temporary stream and checksum what it
 * produced.
 *
 * Going through the savegame tables rather than hashing the structs directly is
 * deliberate: Object embeds a ScriptEngine holding two pointers, which differ
 * between processes, and struct padding is not guaranteed to be initialised.
 * The savegame writes named fields, big endian, and converts the script pointer
 * to an offset -- so the bytes are comparable across runs, builds and
 * platforms.
 */
static uint32 MpSync_ChunkCrc(bool (*saveProc)(FILE *fp), bool *ok)
{
	uint8 buffer[4096];
	uint32 crc = 0;
	size_t got;
	FILE *fp;

	fp = tmpfile();
	if (fp == NULL) {
		*ok = false;
		return 0;
	}

	if (!saveProc(fp)) {
		*ok = false;
		fclose(fp);
		return 0;
	}

	rewind(fp);
	while ((got = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
		crc = MpSync_Crc32(crc, buffer, (uint32)got);
	}

	fclose(fp);
	return crc;
}

/** Pack a value little endian, so the checksum does not depend on the host. */
static void MpSync_PutU32(uint8 *dst, uint32 value)
{
	dst[0] = (uint8)((value >>  0) & 0xFF);
	dst[1] = (uint8)((value >>  8) & 0xFF);
	dst[2] = (uint8)((value >> 16) & 0xFF);
	dst[3] = (uint8)((value >> 24) & 0xFF);
}

/**
 * Take a checksum of the entire simulation state.
 *
 * @param checksum Filled in with one CRC per savegame chunk, plus the random
 *   generators and a combined value.
 * @return True if every chunk could be written.
 */
bool MpSync_Take(MpSyncChecksum *checksum)
{
	uint8 packed[8 * 4];
	bool ok = true;

	memset(checksum, 0, sizeof(*checksum));

	checksum->info      = MpSync_ChunkCrc(&Info_Save,      &ok);
	checksum->house     = MpSync_ChunkCrc(&House_Save,     &ok);
	checksum->unit      = MpSync_ChunkCrc(&Unit_Save,      &ok);
	checksum->structure = MpSync_ChunkCrc(&Structure_Save, &ok);
	checksum->map       = MpSync_ChunkCrc(&Map_Save,       &ok);
	checksum->team      = MpSync_ChunkCrc(&Team_Save,      &ok);
	checksum->unitNew   = MpSync_ChunkCrc(&UnitNew_Save,   &ok);

	/* Neither generator is part of the savegame, and the random stream is
	 * exactly what a desync travels through, so it gets its own component. */
	MpSync_PutU32(packed + 0, Tools_Random_GetSeed());
	MpSync_PutU32(packed + 4, Tools_RandomLCG_GetSeed());
	checksum->rng = MpSync_Crc32(0, packed, 8);

	MpSync_PutU32(packed +  0, checksum->info);
	MpSync_PutU32(packed +  4, checksum->house);
	MpSync_PutU32(packed +  8, checksum->unit);
	MpSync_PutU32(packed + 12, checksum->structure);
	MpSync_PutU32(packed + 16, checksum->map);
	MpSync_PutU32(packed + 20, checksum->team);
	MpSync_PutU32(packed + 24, checksum->unitNew);
	MpSync_PutU32(packed + 28, checksum->rng);
	checksum->total = MpSync_Crc32(0, packed, sizeof(packed));

	return ok;
}

/**
 * Render one checksum as a single line, in the order the components are most
 * likely to diverge.
 */
void MpSync_Format(char *dst, uint16 size, uint32 tick, const MpSyncChecksum *checksum)
{
	snprintf(dst, size,
	         "mp-checksum t%-7u total %08x  unit %08x str %08x house %08x map %08x team %08x new %08x info %08x rng %08x",
	         (unsigned)tick,
	         (unsigned)checksum->total,
	         (unsigned)checksum->unit, (unsigned)checksum->structure,
	         (unsigned)checksum->house, (unsigned)checksum->map,
	         (unsigned)checksum->team, (unsigned)checksum->unitNew,
	         (unsigned)checksum->info, (unsigned)checksum->rng);
}
