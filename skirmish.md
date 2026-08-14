# Skirmish — AI vs AI

A skirmish is a generated match on a 62x62 map between two AI houses, watched by
a human spectator. It exists to develop and judge the AI: nothing in it is meant
to be played, and the first iteration deliberately stops at base construction.

It has no main menu entry on purpose -- it is a development tool, not something
to play. Start it from the command line:

```bash
cd bin
./opendune --skirmish                        # Atreides vs Harkonnen
./opendune --skirmish=ordos,harkonnen        # any two different Houses
```

House names are the ones in `g_table_houseInfo` (`harkonnen`, `atreides`,
`ordos`, `fremen`, `sardaukar`, `mercenary`), case insensitive. The two must
differ: everything in the engine is keyed on the House index. Bad input warns and
falls back to the default pair. The same flag also picks the Houses for
`--skirmish-self-test`.

## The spectator

`g_playerHouseID` is `HOUSE_MERCENARY`, a house that owns nothing. This is not
cosmetic: nearly every AI branch in the engine is written as
`houseID != g_playerHouseID` ([src/structure.c:441](src/structure.c:441) is the
important one), so a house that *is* the player silently stops behaving like an
AI. Attaching the camera to one of the two AIs would mean rewriting all of them.

Consequences, all handled in [src/skirmish.c](src/skirmish.c) and its two
callers:

* the whole map is unveiled at start (`isUnveiled = true` for all 4096 tiles,
  `Game_Prepare()` then builds the fog overlay from it);
* `House_UpdateRadarState()` forces the radar on — the spectator has no Outpost
  and no power, and the minimap is gated on both;
* `winFlags`/`loseFlags` stay zero, so `GameLoop_IsLevelFinished()` never fires.

## The base plan

Each AI is given **only a Construction Yard**. Everything else is a plan: an
ordered list of (structure type, packed position) laid out at start inside a
20x16 rock plateau carved out of the generated landscape. The Construction Yard
works through it, which reuses the engine's own rebuild mechanism
(`House.ai_structureRebuild`, five slots) rather than replacing it:

| Hook | Where | What it does |
|---|---|---|
| `Skirmish_Plan_PickNext()` | `Structure_AI_PickNextToBuild()` ([src/structure.c:2121](src/structure.c:2121)) | picks the next plan entry once the rebuild slots are empty |
| `Skirmish_Plan_TakePosition()` | the AI placement branch ([src/structure.c:364](src/structure.c:364)) | hands out the position the finished structure was planned for |
| `Skirmish_House_MaxCredits()` | `GameLoop_House()` | credit ceiling, see below |
| `Skirmish_AI_WantsHarvester()` | `Structure_AI_PickNextToBuild()` | lets the Heavy Factory build harvesters |

Rebuilding a lost structure always wins over advancing the plan — that is the
stock behaviour and it is the right priority.

### What the picker actually decides

`Structure_GetBuildable()` skips the tech tree entirely for AI houses
([src/structure.c:2046](src/structure.c:2046)): the condition is
`(structuresBuilt & structuresRequired) == structuresRequired || houseID != g_playerHouseID`.
So the stock AI has no build order at all — it takes whatever its rebuild slots
list first. `Skirmish_Plan_PickNext()` therefore enforces the order itself:

1. `structuresRequired` must be satisfied — the plan is what a human would call
   a build order, and it has to hold up;
2. a Windtrap jumps the queue while `powerProduction < powerUsage + 20`, because
   a house in deficit caps every structure at half hitpoints;
3. an entry we cannot pay for is skipped in favour of a cheaper one, otherwise
   the whole queue stalls behind one expensive building.

Rules 2 and 3 are the tuning surface. Rule 3 in particular is crude: it happily
builds a Silo before the Light Factory that unlocks the Heavy Factory.

## Engine facts worth remembering

* **Credit clamp.** `GameLoop_House()` clamps every non-player house to its
  spice storage, which is zero for a house that owns only a Construction Yard —
  the starting credits would be wiped before the first Windtrap is paid for.
  Skirmish houses get the same grace the human gets (`g_playerCreditsNoSilo`):
  they may hold `SKIRMISH_START_CREDITS` until real storage exceeds it.
* **`buildable` was truncated.** `Structure_AI_PickNextToBuild()` stored the
  32 bit mask from `Structure_GetBuildable()` in a `uint16`, dropping every
  structure type above 15 — Rocket Turret (16), Silo (17) and Outpost (18). The
  AI could never build or rebuild those; fixing the type is what made the plan
  progress past the Refinery. This affects the campaign AI too.
* **Harvesters.** The stock AI masks the harvester out of the Heavy Factory, so
  a house lives on the single free harvester its Refinery came with. In skirmish
  it builds up to `SKIRMISH_HARVESTER_TARGET` (3); without this the plan stalls
  around 10 structures with zero credits.
* **Placement is unchecked for AI houses.** `Structure_Place()` only rejects
  invalid locations for the player ([src/structure.c:664](src/structure.c:664)),
  and `Structure_IsValidBuildLocation()` tests adjacency against
  *`g_playerHouseID`*'s buildings, so it is useless for planning. The plateau is
  carved to rock precisely so the plan does not have to rely on it.

## Attacks

Teams are what makes an AI attack: on their own, factory units roll out and
guard. Each House gets four (`s_teamPlan` in [src/skirmish.c](src/skirmish.c)) —
one foot, one wheeled, two tracked, since a team only ever recruits units that
move the way it does. `TEAM_ACTION_NORMAL` in `TEAM.EMC` recruits the closest
free unit, gathers when it is below its minimum, picks a target and orders
everyone onto it.

Three engine facts had to give way first, all of them consequences of there
being no human on the map:

* **Every AI was allied with every other AI.** `House_AreAllied()` ends with
  `houseID1 != g_playerHouseID && houseID2 != g_playerHouseID`: Dune II only ever
  has one enemy, the player, so any two other Houses are friends by definition.
  With a spectator as the player, the two skirmish AIs were allies — every target
  priority scored 0 and both armies stood around doing nothing, however long you
  watched. A skirmish makes them enemies.
* **Teams only recruit scenario units.** `Script_Team_AddClosestUnit()` skips
  anything without `byScenario`, so in the campaign a team is filled from the INI
  and from reinforcements — factory output never joins one. A skirmish has no
  scenario to draw on, so the filter is lifted while a skirmish is running.
* **Nobody had ever seen anything.** `Unit_GetTargetStructurePriority()` and its
  unit counterpart return 0 for a target the House has not seen, and being seen
  is a side effect of the *human* unveiling the map. `Structure_Place()` already
  marks the player's own base as seen by everyone — that is what gives the
  campaign AI something to attack — so in a skirmish both AI bases take that
  role, and units are marked the same way in `Unit_SetPosition()`.

A fourth thing only surfaced once shots were actually fired: `Voice_PlayAtTile()`
memmoves through `g_readBuffer`, which the main menu allocates on its way into a
game. Both skirmish entry points skip the menu, so the first shot fired
segfaulted. The buffer is now allocated for them, and the voice player falls back
to the sound effect when it has none.

What this produces today: bases take real damage and buildings do fall, but the
loser is usually starved first — harvesters die, the economy stops, the plan
stalls. Attackers still arrive piecemeal and feed the turrets. Staging before
committing, keeping a garrison at home and target selection are the next things
to work on.

## Concrete

`Skirmish_LaySlabs()` pours concrete over a structure's footprint just before it
is placed, and bills the House 5 credits per tile. The AI itself has no notion of
slabs — it only ever orders whole structures — so the plan does it.

Note what this does *not* fix: `Structure_IsValidBuildLocation()` decides the
missing-slab penalty by looking for buildings belonging to `g_playerHouseID`, so
for an AI house it returns 0 (invalid) and `Structure_Place()` then skips both
the hitpoint penalty and the degrades flag. AI structures therefore never eroded,
concrete or not. Making that check house-aware would make the concrete count —
and would also make every campaign AI base start degrading, which is why it has
not been done.

## Economy

Two things had to change before spice actually kept flowing.

* **The harvester recovery layer was player-only.** `Unit_Harvester_Update()`
  ([src/unit.c:1874](src/unit.c:1874)) returned immediately for anyone but
  `g_playerHouseID` — in the campaign nobody watches the AI closely enough to
  care. Its harvesters stall the same way the player's used to: a route that
  never makes progress leaves the script sitting in `ACTION_HARVEST` with no
  destination, and that harvester never moves again. In a skirmish the layer
  covers every house, and the rest of it is already house-agnostic (see
  [harvester.md](harvester.md)).
* **The map ran dry.** `Map_CreateLandscape()` scatters up to 47 small fields,
  which suits a campaign scenario; two AIs mining continuously strip that within
  a few thousand ticks, and Dune II has no spice regrowth. `Skirmish_SeedSpice()`
  lays 60 more fields at start plus two just outside each base, so the opening is
  not decided by which corner the generator favoured. Watch the `spice` figure in
  the self-test trace: when it stops falling, harvesting has stopped.

## Watching a match

Two lines are drawn over the top of the viewport, one per AI:

```
Atreides 1401c 300/115p h2 u17 s10@94% 10/20 Heavy Fctry
```

house, credits, power production/usage, harvesters, units, standing structures
and their share of full hitpoints, plan progress, and what the Construction Yard
is building right now.

The simulation speed is shown in the top right corner of the tactical view. `[`
halves it and `]` doubles it, up to eight extra passes on top of the Game
Controls speed setting (so x16 at most).

## The self-test

```bash
cd bin
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --skirmish-self-test=200000
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --skirmish=ordos,atreides --skirmish-self-test=200000
```

Runs a headless match by driving `Timer_AdvanceGame()` and
the four `GameLoop_*` subsystems by hand — the real loop is paced by the GUI, and
a base takes minutes of wall clock. It prints the summary line and the full build
order per house, which is the thing to read when judging a change:

```
t100000 Atreides 0c 300/140p h3 u18 s11@100% 10/20 Turret
Harkonnen 2005c 500/445p h2 u26 s22@98% 21/21 -
Windtrap, Refinery, Outpost, Windtrap, Barracks, WOR, Spice Silo, Turret, ...
teams: foot 8/4-8, wheel 4/2-4, track 1/3-6, track 1/3-6
```

The `t<tick>` lines are a trace printed five times during the run, prefixed with
the spice left on the map: base growth, losses and the economy only mean
something as a curve. A `*` after a team's size means it
currently holds a target.

The tick count is the knob: 200000 ticks is roughly a completed plan on a map
with spice near both bases. The map is random, so a house can genuinely be
starved — a run that stops at 10/20 with 0 credits is data, not necessarily a
regression. Run it twice before believing it.

## The economy search

`--economy-search` reuses everything here with combat off and a single house, to
evolve the fastest economic opening — see [economy.md](economy.md). Two skirmish
features exist for it: `Skirmish_StartEconomy()` (solo, fixed seed, plan from the
search) and the Starport ordering the stock AI never had.

## Not done yet

* Attacks are piecemeal: a team walks in as it recruits instead of massing first,
  and nothing is held back to defend the base.
* Tracked teams stay near empty — the unit cap fills with infantry from the
  Barracks long before the Heavy Factory is up.
* Turrets are packed into the base rows like everything else instead of being
  placed on the perimeter.
* A skirmish is not saveable — the plan lives in `src/skirmish.c` statics and is
  not part of the savegame.
