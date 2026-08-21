/** @file src/mpsync.c Simulation state checksum, for desync hunting. */

#include <stdio.h>
#include <string.h>
#include "types.h"

#include "mpsync.h"

#include "saveload/saveload.h"
#include "scenario.h"
#include "opendune.h"
#include "house.h"
#include "unit.h"
#include "gui/gui.h"
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

/**
 * The info chunk, without the six numbers that belong to whoever is watching.
 *
 * killed/destroyed/harvested are each split into "allied" and "enemy", and the
 * score is signed by the same test -- which side is which is a question only a
 * viewpoint can answer -- so in a match the two
 * clients keep two different tallies of the same events, correctly.  They are
 * a post-match readout and feed nothing, but they ride in the savegame's info
 * chunk, so a desync check that included them would report a disagreement on
 * every kill.  Stashed and restored around the write rather than removed from
 * the chunk: the format stays the savegame's.
 */
/**
 * The INFO chunk, with everything that only describes one player taken out.
 *
 * Two kinds of thing live in there and only one of them is the match. The
 * mission tallies and the score are per client because each client is keeping
 * score for its own house. The selection, the active action, the building being
 * placed and where the minimap is pointing are per client because they are the
 * chair, not the world -- and every one of them is saved, so a checksum that
 * counted them would call two people watching the same match a desync. The
 * fields that are genuinely shared -- the scenario, the clock, the starport
 * stock, the missile countdown -- stay in.
 */
static bool MpSync_InfoSave(FILE *fp)
{
	uint16 killedAllied     = g_scenario.killedAllied;
	uint16 killedEnemy      = g_scenario.killedEnemy;
	uint16 destroyedAllied  = g_scenario.destroyedAllied;
	uint16 destroyedEnemy   = g_scenario.destroyedEnemy;
	uint16 harvestedAllied  = g_scenario.harvestedAllied;
	uint16 harvestedEnemy   = g_scenario.harvestedEnemy;
	int16  score            = g_scenario.score;

	uint16 creditsNoSilo    = g_playerCreditsNoSilo;
	uint16 minimapPosition  = g_minimapPosition;
	uint16 selectionRect    = g_selectionRectanglePosition;
	uint16 selectionType    = g_selectionType;
	uint16 activeType       = g_structureActiveType;
	uint16 activePosition   = g_structureActivePosition;
	Structure *activeStructure = g_structureActive;
	Unit *unitSelected      = g_unitSelected;
	Unit *unitActive        = g_unitActive;
	uint16 activeAction     = g_activeAction;
	bool ret;

	g_scenario.killedAllied = g_scenario.killedEnemy = 0;
	g_scenario.destroyedAllied = g_scenario.destroyedEnemy = 0;
	g_scenario.harvestedAllied = g_scenario.harvestedEnemy = 0;
	g_scenario.score = 0;

	g_playerCreditsNoSilo = 0;
	g_minimapPosition = 0;
	g_selectionRectanglePosition = 0;
	g_selectionType = 0;
	/* 0xFFFF, not 0: "nothing is being placed" has a sentinel here, and 0 is
	 * a real structure type.  Info_Save() reads the pointer whenever the type
	 * says something is being placed, so zeroing the pair is a null dereference
	 * rather than a clear. */
	g_structureActiveType = 0xFFFF;
	g_structureActivePosition = 0;
	g_structureActive = NULL;
	g_unitSelected = NULL;
	g_unitActive = NULL;
	g_activeAction = 0;

	ret = Info_Save(fp);

	g_playerCreditsNoSilo = creditsNoSilo;
	g_minimapPosition = minimapPosition;
	g_selectionRectanglePosition = selectionRect;
	g_selectionType = selectionType;
	g_structureActiveType = activeType;
	g_structureActivePosition = activePosition;
	g_structureActive = activeStructure;
	g_unitSelected = unitSelected;
	g_unitActive = unitActive;
	g_activeAction = activeAction;

	g_scenario.killedAllied    = killedAllied;
	g_scenario.killedEnemy     = killedEnemy;
	g_scenario.destroyedAllied = destroyedAllied;
	g_scenario.destroyedEnemy  = destroyedEnemy;
	g_scenario.harvestedAllied = harvestedAllied;
	g_scenario.harvestedEnemy  = harvestedEnemy;
	g_scenario.score           = score;

	return ret;
}

/**
 * Write every chunk to one file, for when a checksum has said *that* two
 * clients disagree and the question is *what* about.
 *
 * Deliberately the same writers the checksum uses, so the bytes compared here
 * are the bytes that were hashed there.  Two dumps taken at the same tick on
 * two clients and run through cmp name the chunk, the offset inside it and --
 * divided by the record size -- the object.
 */
bool MpSync_Dump(const char *path)
{
	static const char *names[] = { "info", "house", "unit", "structure", "map", "team", "unitnew" };
	static bool (* const savers[])(FILE *fp) = {
		&Info_Save, &House_Save, &Unit_Save, &Structure_Save, &Map_Save, &Team_Save, &UnitNew_Save
	};
	uint8 buffer[4096];
	FILE *out;
	uint16 i;

	out = fopen(path, "wb");
	if (out == NULL) return false;

	for (i = 0; i < 7; i++) {
		FILE *fp = tmpfile();
		size_t got;
		char header[64];
		long size;

		if (fp == NULL) continue;

		if (!savers[i](fp)) {
			fclose(fp);
			continue;
		}

		fseek(fp, 0, SEEK_END);
		size = ftell(fp);
		rewind(fp);

		snprintf(header, sizeof(header), "\n== %s %ld\n", names[i], size);
		fwrite(header, 1, strlen(header), out);

		while ((got = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
			fwrite(buffer, 1, got, out);
		}

		fclose(fp);
	}

	fclose(out);
	return true;
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

	checksum->info      = MpSync_ChunkCrc(&MpSync_InfoSave, &ok);
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
