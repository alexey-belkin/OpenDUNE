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
#include "pool/unit.h"
#include "pool/structure.h"
#include "team.h"
#include "tools.h"

static bool MpSync_UnitSave(FILE *fp);
static bool MpSync_StructureSave(FILE *fp);

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

	uint32 hintsShown1      = g_hintsShown1;
	uint32 hintsShown2      = g_hintsShown2;
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

	/* Which hints this player has already been shown, remembered in the
	 * savegame.  It is a note about the person, not about the world. */
	g_hintsShown1 = 0;
	g_hintsShown2 = 0;
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

	g_hintsShown1 = hintsShown1;
	g_hintsShown2 = hintsShown2;
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
		&MpSync_InfoSave, &House_Save, &MpSync_UnitSave, &MpSync_StructureSave, &Map_Save, &Team_Save, &UnitNew_Save
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

/*
 * The clamp: drawing is not allowed to change the world.
 *
 * Every viewpoint leak found so far has been one shape -- something on the
 * presentation side of a line this code does not have wrote something the
 * savegame stores.  They were all found the expensive way: two processes, a
 * relay, a divergence noticed a minute later with the cause long off screen,
 * then an evening of bisecting samples.
 *
 * That was the wrong detector for the question.  The question is not "do two
 * machines agree" -- it is "did that function change the world", and one
 * machine can answer it, in the same frame, with the name of the function.
 * Take a checksum, run the suspect, take it again: if they differ, the suspect
 * is impure and the chunk says where to look.
 *
 * It is the write barrier a garbage collector uses, or the borrow checker's
 * question, done cheaply in C for exactly one invariant.
 */
/*
 * Two independent regions, because two different questions are worth asking.
 * The coarse one spans everything between two simulation steps -- if the world
 * moved there, something outside the simulation moved it, whatever that
 * something was.  The fine ones name a suspect: this draw call, this click.
 * The coarse one is the safety net, and it is the one that caught what the
 * fine ones were not wrapped around.
 */
enum {
	MP_PURITY_COARSE = 0,
	MP_PURITY_FINE   = 1,
	MP_PURITY_SLOTS  = 2
};

static struct {
	bool enabled;
	struct {
		bool armed;
		MpSyncChecksum before;
		uint32 reported;                                /*!< One report per chunk, or a bad frame prints for ever. */
	} slot[MP_PURITY_SLOTS];
} s_purity;

void MpPurity_SetEnabled(bool enabled)
{
	s_purity.enabled = enabled;
}

bool MpPurity_IsEnabled(void)
{
	return s_purity.enabled;
}

/* When the chunk name is not enough, keep the bytes too: the first report then
 * leaves a before/after pair on disk and the offset names the field. */
static bool s_purityDump = false;
static bool s_purityDumped = false;

void MpPurity_SetDump(bool dump)
{
	s_purityDump = dump;
}

void MpPurity_Begin(uint16 slot)
{
	if (!s_purity.enabled || slot >= MP_PURITY_SLOTS) return;

	s_purity.slot[slot].armed = MpSync_Take(&s_purity.slot[slot].before);

	if (s_purityDump && !s_purityDumped && slot == MP_PURITY_COARSE) {
		MpSync_Dump("purity-before.bin");
	}
}

/**
 * Take the "before" again, mid region.
 *
 * The match pump steps the simulation from inside sleepIdle(), and the modal
 * screens this clamp most wants to watch are full of it -- so the world moves
 * inside the region for a legitimate reason.  Abandoning the measurement there
 * was the first idea and it was wrong: it blinds the clamp for exactly the
 * regions that need it, and a hint popup that writes a saved field went
 * unreported because a pump had fired first.  Rebasing keeps the region under
 * watch and only forgives what the pump did.
 */
void MpPurity_Rebase(uint16 slot)
{
	if (!s_purity.enabled || slot >= MP_PURITY_SLOTS) return;
	if (!s_purity.slot[slot].armed) return;

	s_purity.slot[slot].armed = MpSync_Take(&s_purity.slot[slot].before);
}

/**
 * @return True if the world is unchanged, which is the only acceptable answer.
 */
bool MpPurity_End(uint16 slot, const char *what, uint32 tick)
{
	static const char *names[] = { "info", "house", "unit", "structure", "map", "team", "unitnew", "rng" };
	MpSyncChecksum after;
	uint32 before[8];
	uint32 now[8];
	uint16 i;
	bool clean = true;

	if (!s_purity.enabled || slot >= MP_PURITY_SLOTS) return true;
	if (!s_purity.slot[slot].armed) return true;

	s_purity.slot[slot].armed = false;

	if (!MpSync_Take(&after)) return true;
	if (after.total == s_purity.slot[slot].before.total) return true;

	before[0] = s_purity.slot[slot].before.info;      now[0] = after.info;
	before[1] = s_purity.slot[slot].before.house;     now[1] = after.house;
	before[2] = s_purity.slot[slot].before.unit;      now[2] = after.unit;
	before[3] = s_purity.slot[slot].before.structure; now[3] = after.structure;
	before[4] = s_purity.slot[slot].before.map;       now[4] = after.map;
	before[5] = s_purity.slot[slot].before.team;      now[5] = after.team;
	before[6] = s_purity.slot[slot].before.unitNew;   now[6] = after.unitNew;
	before[7] = s_purity.slot[slot].before.rng;       now[7] = after.rng;

	for (i = 0; i < 8; i++) {
		char line[192];

		if (before[i] == now[i]) continue;

		clean = false;

		if ((s_purity.slot[slot].reported & (1 << i)) != 0) continue;
		s_purity.slot[slot].reported |= (1 << i);

		snprintf(line, sizeof(line), "sim-purity: %s changed %s at tick %u (%08x -> %08x)",
		         what, names[i], (unsigned)tick, (unsigned)before[i], (unsigned)now[i]);
		MpPurity_Report(line);

		if (s_purityDump && !s_purityDumped) {
			s_purityDumped = true;
			MpSync_Dump("purity-after.bin");
		}
	}

	return clean;
}

/*
 * isDirty and isHighlighted are instructions to the renderer -- "repaint me",
 * "draw me lit" -- and they live in the object flags, which are saved.  So the
 * savegame records which sprites this particular screen needed to redraw, and
 * two clients drawing two different views need to redraw different things.
 * Every unit on the map differed in exactly this one bit.
 *
 * They come out of the checksum the same way the selection and the score did.
 */
static uint8 s_flagBackupUnit[UNIT_INDEX_MAX];
static uint8 s_flagBackupStructure[STRUCTURE_INDEX_MAX_HARD];

static uint8 MpSync_TakeDrawFlags(ObjectFlags *flags)
{
	uint8 saved = (uint8)((flags->s.isDirty ? 1 : 0) | (flags->s.isHighlighted ? 2 : 0));

	flags->s.isDirty       = false;
	flags->s.isHighlighted = false;

	return saved;
}

static void MpSync_PutDrawFlags(ObjectFlags *flags, uint8 saved)
{
	flags->s.isDirty       = (saved & 1) != 0;
	flags->s.isHighlighted = (saved & 2) != 0;
}

static bool MpSync_UnitSave(FILE *fp)
{
	uint16 i;
	bool ret;

	for (i = 0; i < UNIT_INDEX_MAX; i++) {
		Unit *u = Unit_Get_ByIndex(i);

		if (u == NULL) continue;
		s_flagBackupUnit[i] = MpSync_TakeDrawFlags(&u->o.flags);
	}

	ret = Unit_Save(fp);

	for (i = 0; i < UNIT_INDEX_MAX; i++) {
		Unit *u = Unit_Get_ByIndex(i);

		if (u == NULL) continue;
		MpSync_PutDrawFlags(&u->o.flags, s_flagBackupUnit[i]);
	}

	return ret;
}

static bool MpSync_StructureSave(FILE *fp)
{
	uint16 i;
	bool ret;

	for (i = 0; i < STRUCTURE_INDEX_MAX_HARD; i++) {
		Structure *st = Structure_Get_ByIndex(i);

		if (st == NULL) continue;
		s_flagBackupStructure[i] = MpSync_TakeDrawFlags(&st->o.flags);
	}

	ret = Structure_Save(fp);

	for (i = 0; i < STRUCTURE_INDEX_MAX_HARD; i++) {
		Structure *st = Structure_Get_ByIndex(i);

		if (st == NULL) continue;
		MpSync_PutDrawFlags(&st->o.flags, s_flagBackupStructure[i]);
	}

	return ret;
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
	checksum->unit      = MpSync_ChunkCrc(&MpSync_UnitSave,      &ok);
	checksum->structure = MpSync_ChunkCrc(&MpSync_StructureSave, &ok);
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
