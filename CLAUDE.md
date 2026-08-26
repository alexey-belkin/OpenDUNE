# OpenDUNE — working notes

Open source re-creation of Westwood's **Dune II**. This fork adds modern RTS
conveniences (group selection, order queue, autonomous defence, harvester
management) on top of the original engine.

## The one thing to understand first

The engine is ours; **the game logic is not**. Unit, structure and team
behaviour lives in Westwood's `UNIT.EMC` / `BUILD.EMC` / `TEAM.EMC` bytecode
inside `DUNE.PAK`, and OpenDUNE interprets it. `Unit_SetAction()` loads bytecode,
it does not call a C function. C code can only supplement the script between its
ticks, which makes field ownership the central discipline here.

→ [emc-scripts.md](emc-scripts.md) — the VM, the disassembler, and the ownership
rules for shared state. **Read it before touching anything behavioural.**

## Layout

| Path | Contents |
|---|---|
| `src/` | engine: `unit.c`, `structure.c`, `map.c`, `house.c`, `skirmish.c`, `opendune.c` (main loop) |
| `src/script/` | the EMC virtual machine and its opcode implementations |
| `src/gui/` | viewport, widgets, input, drawing |
| `src/table/` | static data: `unitinfo.c`, `structureinfo.c`, `actioninfo.c` |
| `src/skirmish.c` `src/ecosearch.c` `src/warsearch.c` | the AI test bench: match setup, the economy search, the war search |
| `src/saveload/` | save format — extending a struct means touching this |
| `tools/` | asset extractors, `dis_emc.py` for the game scripts, `telemetry_report.py` for recorded matches, `threat_report.py` for the combat balance model, `relay/` (the multiplayer relay, Go) |
| `bin/` | build output and `bin/data/` (game files) |
| `bundle/` | `make bundle` output — **wiped on every build** |

## Build and run

```bash
make -j8                      # → bin/opendune, expects game data in bin/data/
make bundle                   # → bundle/OpenDUNE.app  (does `rm -rf bundle/` first)
```

Test with `bundle/OpenDUNE.app`; the revision shown in the main menu identifies
the build (`g<sha>`, plus `M` when built from a dirty tree). Do not test with
`/Applications/OpenDUNE.app` unless you copied the binary there — `make install`
is a no-op on macOS.

**`make bundle` deletes `bundle/` wholesale.** Game data placed inside the app
bundle does not survive it, so restore it afterwards:

```bash
mkdir -p bundle/OpenDUNE.app/Contents/Resources/data
cp -p bin/data/*.PAK bin/data/DUNE.CFG bundle/OpenDUNE.app/Contents/Resources/data/
```

## Shipping a build to another machine

```bash
tools/package.sh              # → bundles/opendune-<rev>-<date>-macos-arm64.zip
```

`make bundle` on its own does not produce something another Mac can run. Two
reasons, and the script exists for both: the app has no game data in it (the
`rm -rf bundle/` above took it out), and the binary still names
`/opt/homebrew/opt/sdl2/...` in its load commands, so on a machine without that
Homebrew prefix it dies in dyld before a window appears. `package.sh` copies the
data back, vendors `libSDL2` into `Contents/Frameworks/`, rewrites the load
command to `@executable_path/../Frameworks/`, re-signs ad-hoc (an
`install_name_tool` edit invalidates the signature, and arm64 will not load an
image whose signature does not match), and refuses to write the archive if
anything is still linked outside the bundle.

Then it verifies what it is about to hand over rather than the tree it came
from: the zip is unpacked into a scratch directory and
`--combat-balance-self-test`, `--selection-self-test` and `--mp-replay` are run
from *that* copy. A package that fails is deleted rather than shipped. Rerun it
after every change that a second machine is meant to see — that is the whole
point of it being one command.

The archive is named after the architecture it actually contains, and
`INSTALL.txt` states that and the **minimum macOS**, which is the thing that
really stops an older machine from opening it. Watch that number: nothing sets
a deployment target, so clang stamps the host's version — a build made here
today refuses to launch on anything older than macOS 26, Apple Silicon
included. `--no-data` leaves the `*.PAK` out and says so in `INSTALL.txt`;
`--no-build` packages whatever `bin/` already holds.

**An Intel package** is a separate build, not a repackage, and Homebrew has no
Intel bottles any more — so SDL2 has to be built from source for x86_64 first.
Statically, which is better than the arm64 path: nothing is left to vendor, no
load command to rewrite, no signature to repair.

```bash
curl -fsSLO https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz
tar xzf SDL2-2.32.10.tar.gz && cd SDL2-2.32.10
./configure --host=x86_64-apple-darwin --prefix=/tmp/sdl-x86_64 \
  CC="clang -arch x86_64 -mmacosx-version-min=10.13" \
  LDFLAGS="-arch x86_64 -mmacosx-version-min=10.13"
make -j8 && make install

git worktree add /tmp/x86tree HEAD && cd /tmp/x86tree
cp -p <repo>/bin/data/*.PAK <repo>/bin/data/DUNE.CFG bin/data/
./configure --with-sdl2=/tmp/sdl-x86_64/bin/sdl2-config --enable-static \
  CC="clang -arch x86_64 -mmacosx-version-min=10.13"
tools/package.sh
```

A worktree rather than the main tree because `./configure` overwrites
`Makefile.config` in place, and the two architectures cannot share it. The
result runs on any Intel Mac from High Sierra onwards and, through Rosetta, on
this machine — which is how `package.sh` verifies it.

**Then check that the two packages can play each other**, because nothing else
does. Build both from the same commit with both trees equally clean, and compare
the room name each one derives:

```bash
cd <repo>/bin      && ./opendune --lobby-play=127.0.0.1:1,x,0,1 2>&1 | grep 'mp-live: room'
cd /tmp/x86tree/bin && ./opendune --lobby-play=127.0.0.1:1,x,0,1 2>&1 | grep 'mp-live: room'
```

The digest in the two strings must be identical. It is a hash of the balance
tables and the revision — see "The lobby" — and if it differs, an Intel player
and an Apple Silicon player are put in different rooms and simply never find
each other. That has already happened twice for two different reasons, and both
times only this comparison could see it.

## Game data

Original Dune II files (`*.PAK`, ~13 MB) live in `bin/data/`, gitignored. They
are required: without them the game cannot start. A second copy is in
`/Applications/OpenDUNE.app/Contents/Resources/data/`, which no build touches —
useful as the recovery source.

## Combat balance — the `opendune.ini` tuning module

Unit damage is tunable at runtime from an external ini, without rebuilding. All
keys live under the `[opendune]` heading of `opendune.ini`; `bin/opendune.ini.sample`
is the annotated template and README.txt ("Combat class balance") is the
user-facing description. The file is searched in this order
([src/inifile.c:39](src/inifile.c:39)): `~/Library/Application Support/OpenDUNE/`
(macOS), then the current directory, then `data/`, then the directory *next to*
`OpenDUNE.app`. **A copy exists on this machine**, at
`~/Library/Application Support/OpenDUNE/opendune.ini`, and because that is the
*first* location searched it shadows anything put in `bin/`. It currently
overrides six keys — `class_range_p_bonus=2`, `class_damage_p_vs_rp=500`,
`class_damage_rp_vs_lt=200`, `class_damage_rp_vs_tt=300`,
`class_damage_lt_vs_rp=175` and `tech_tree=mp` — so **every run on this machine,
`--war-metrics` and `tools/mpduel.sh` included, is played on those numbers and
on the fork's tech tree, not on the compiled-in defaults**.
`tools/threat_report.py` reads the source, not the ini, so it is describing a
configuration this machine does not run.

To measure against the compiled-in defaults, point `HOME` somewhere empty and put
the ini you want in `bin/`:

```bash
HOME=/tmp/emptyhome SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune ...
```

The module itself is `Unit_CombatBalance_*` in [src/unit.c:65](src/unit.c:65):

* `Unit_CombatBalance_Init()` — called once from `opendune.c`; reads every key,
  clamps percentages to 0..1000, and patches `g_table_unitInfo` /
  `g_table_structureInfo` (infantry range, light infantry speed, infantry
  factories open to every House). It runs immediately after
  `ReadProfileIni()`, so it wins over the `[combat]` section of `PROFILE.INI`.
  The
  compiled-in defaults are the `s_combatBalance` initialiser — they are the
  fallback for every missing key, so **defaults live in two places** and the
  sample ini must be kept in step with them.
* `Unit_CombatBalance_ApplyHouseDamage()` — House identity bonus, applied to the
  base shot in [src/script/unit.c:664](src/script/unit.c:664), i.e. it also
  affects damage to structures.
* `Unit_CombatBalance_ApplyClassDamage()` — the 4x4 attacker/target matrix,
  applied at impact in [src/map.c:435](src/map.c:435), unit-versus-unit only.

Classes are P (Soldier, Infantry), RP (Trooper, Troopers), LT (Trike, Raider
Trike, Quad), TT (Tank, Siege Tank, Devastator) and AR (Launcher, Sonic Tank —
the two units that outrange a Rocket Turret, per `Doctrine_RoleOf()`; the
Saboteur is Ordos' stand-in for them but is not in the class); every other unit
is neutral (x1.00) on both sides of the matrix. Values are integer percentages —
100 is x1.00. Keys: `class_balance_enabled`, `class_balance_infantry_all_houses`
(formerly `class_balance_shared_infantry`, still accepted),
`class_range_p_bonus`, `class_speed_p_over_rp`,
`class_damage_<attacker>_vs_<target>` for the 25 matrix cells, and
`class_bonus_atreides_p` / `class_bonus_harkonnen_rp` /
`class_bonus_ordos_trike` (the Ordos bonus deliberately skips Quad).

Balance decisions that do not follow the class lines live in the `s_unitTuning`
table instead: `unit_damage_<name>` and `unit_rate_<name>`, both percentages of
the table value, with `<name>` the unit's name lowercased and non-letters
collapsed to underscores. Every unit is reachable from the ini; the table only
carries the defaults that are not 100 — currently Sonic Tank at 150% damage,
Siege Tank and Devastator at 115%, Launcher at 150% rate of fire and Raider
Trike at 120%. `unit_rate_*` scales the whole firing cycle including the short
gap inside a `firesTwice` doublet, so 120 means a fifth more shots for every
kind of unit.

**Rate is usually the better lever.** A shot deals a fixed amount and the excess
is lost (`Map_MakeExplosion`), so damage is a step function: it buys nothing
until it crosses the shot count a target dies to. Launcher at 150% damage was
worth exactly zero against Soldier, Trooper, Infantry and Quad — same exchange
rate to two decimals — while 150% rate paid against all twelve unit types. Check
a damage change against `tools/threat_attrition.py --units` before trusting it;
the closed form cannot see the difference (it scored the two Launcher builds at
18.37 and 18.29 DPS).

`class_speed_p_over_rp` is a rule, not a literal: light infantry's
`movingSpeedFactor` is derived from the matching rocket infantry's — Soldier
from Trooper, Infantry from Troopers — so the light half stays the fast half if
the heavy half is retuned. At the default 120 that is Soldier 18 and Infantry
12, which lands light infantry beside the Siege Tank and past the Devastator.
The rationale comment in `Doctrine_RoleOf()` is annotated accordingly: those
units are still `DOCTRINE_ROLE_GARRISON`, but no longer because they cannot
keep up.

`python3 tools/threat_report.py` scores the roster against this configuration,
and `python3 tools/threat_attrition.py` plays the fights out shot by shot when
the closed form is not enough.
It carries no balance numbers of its own — it parses `g_table_unitInfo`, the
`s_combatBalance` matrix and `Unit_CombatBalance_GetClass()` straight out of the
source, so it follows any change made here. Threat is `DPS x hitpoints`, which
is the Lanchester square-law strength, so the price index that goes with it is
`threat / cost^2` rather than `threat / cost`; both are printed, along with the
class-versus-class exchange matrix. `--flat` reruns it with every multiplier
forced to 100 and `--no-tuning` drops `s_unitTuning`, which together separate
what our balance does from what Westwood's stats already did — the useful diff
when retuning a cell.

`threat_attrition.py` buys two squads for a budget and simulates the fight:
whole units, dying one at a time, firepower leaving with them, overkill wasted,
and the `firesTwice` health gate modelled exactly rather than as the closed
form's flat 0.75 (measured, that correction is 0.95–1.00 under focus fire and
0.90–0.98 under spread — the Launcher at 0.78 is the one real exception — so the
closed form under-rates every doublet unit). Fire distribution is the one real choice
and it has exactly two ends: `--spread` and the default focus. `--sonic-tiles N`
sets how many occupied tiles a sonic beam crosses, which is the only thing that
decides whether the Sonic Tank is worthless or the best unit on the field.
`--split AR` and `--units` control how much aggregation the matrix does.

Changing any default means touching three files: the `s_combatBalance`
initialiser, `bin/opendune.ini.sample`, and the README.txt section. Verify with
`--combat-balance-self-test` below, which checks the matrix, the neutral classes,
the House bonuses and the infantry-factory patch against the loaded config.
That patch keeps the two factories separate: Barracks trains Soldier and
Infantry, WOR trains Trooper and Troopers, and both are buildable by every
House.

Its integration step fires a real Atreides Soldier shot (base 10) at a synthetic
Harkonnen Trooper (45 HP), so a strong enough `class_damage_p_vs_rp` makes that
shot lethal and reaches `Unit_SetAction(ACTION_DIE)` with `UNIT.EMC` not yet
loaded — `Sprites_LoadTiles()` only reads it when a scenario loads. The NULL
guard in `Script_Load()` ([src/script/script.c:283](src/script/script.c:283))
exists for exactly that; **rebuild before trusting a segfault here**, a stale
`bin/opendune` predating that guard crashes instead.

## Turning on the move

`move_rolling_turn` (default 1). Arriving on a tile, a unit reads the next
direction off `u->route[]` and, if it differs from where it is pointing, stops
and turns — `Script_Unit_MoveToTarget()` in
[script/unit.c](src/script/unit.c). On a diagonal route that is every other
tile, 45 degrees each time, and it is what makes a column move in jerks. With
the rule on, a one-octant turn that arose **in motion** is set instantly and
`Unit_StartMovement()` runs in the same call, so the unit never stops. Turns
from a standstill and turns of 90 degrees or more are untouched.

"In motion" is `Unit.rollingTurn`, set by `Unit_Move()` where a ground unit
completes a tile and consumed by `Unit_MoveRules_RollingTurn()` —
**consumed whether or not it is used**, which is what stops a unit that halted
here from claiming a free turn when its next order arrives.

The field is a `uint16` for a flag on purpose: it takes the slot that was
reserved at the end of `s_saveUnitNew` ([saveload/unit.c](src/saveload/unit.c)),
so the ODUN chunk is the same length it was and savegames written before this
still load — `--selection-self-test` replays five of them and is the guard.

Measured with `--war-metrics` on the compiled-in defaults, on versus off:
`econ.spice/match` 49520 → 56475, `wave.matches %` 58 → 66, `result.points %`
54 → 66, and `--economy-baseline` 0 from 10311 to 11262. It costs something too:
`turret.entries/match` 12 → 23 and `result.wipeouts %` 25 → 33, because an army
that arrives faster kills harder. `--move-rules-self-test` checks the rule
itself.

## Route finding

`pathfinder_astar` (default 1) replaces Westwood's router with A\* over the
64x64 grid — [pathfinder.c](src/pathfinder.c), called from
`Script_Unit_CalculateRoute()` and `Script_Unit_HasRoute()` in
[script/unit.c](src/script/unit.c). `--pathfinder=0|1` overrides the key from
the command line, which is the only way to A/B on a machine that has an
`opendune.ini` in `~/Library/Application Support/` — that copy is searched first
and shadows anything put in `bin/`.

**The cost function is the whole design.** `Unit_GetTileEnterScore()` still
decides what is *passable* — it knows about allies, transports, conquerable
buildings, the Saboteur and the sandworm — but its *number* is not used. That
number ends in `res ^= 0xFF`, an affine proxy for time, and with
`g_dune2_enhanced` the rates are first scaled by `movingSpeedFactor/256`, which
for ground units is 5/256 to 60/256. Every step therefore lands in 211..252 and
the terrain is worth a few percent of it. Measured properly a Tank crosses sand
in 78 ticks and concrete in 51 — 1.53, reported as 1.06 — and a Soldier's real
ratio is 2.18 against a reported 1.04; diagonals are 1.38 against a reported
1.01. The old router only ever compared two ways round one obstacle so the
distortion never surfaced, but a shortest-path search optimises exactly what it
is given. `Pathfinder_TicksForStep()` therefore reproduces the movement layer:
terrain rate of the tile being **entered** (`Unit_StartMovement()`), the
quantisation in `Unit_SetSpeed()` — above 16 the rate is rounded down to whole
sixteenths of a tile per move, so a Trike at 28 and one at 31 are the same speed
— sixteen-unit moves gated by `Unit_MovementTick()`'s accumulator, arrival at
under 16 on the `max + min/2` metric, and the movement tick running once every
three game ticks.

Three things that are easy to get wrong and are guarded:

* **The heuristic is derived, not chosen.** `Pathfinder_MaxSpeed()` asks the same
  cost function for the fastest ground this unit can be on and builds the octile
  bound from it, so it survives any balance change. The self-test checks it
  against independently computed true distances for **every** tile, not against
  the argument that it ought to be a lower bound.
* **`Unit_GetTileEnterScore()` returns negative numbers.** −1 is "may drive up to
  but not into", −2 is "may enter". The wrapper in script/unit.c maps −1 to 256
  and then lets −2 through *as a negative score*, which was harmless when the
  score only chose between two detours. A search assumes non-negative edges, so
  here −2 is passable and everything else negative is a wall.
* **Ties break on the packed tile index**, never on insertion order or a pointer.
  `Tools_AdjustToGameSpeed()` is deliberately not applied even though the
  movement layer applies it, because it reads a local config value.

`u->route[14]` is unchanged. A\* computes the whole path, the unit takes the
first steps and re-paths when they run out, and each recomputation is optimal in
its own right — widening the buffer would mean the savegame format for no gain.

One behavioural rule rides along, gated on the same key: a unit that is
**itself moving** is traffic, not a wall — passable at double the tile's cost, so
a detour is chosen only when it is genuinely shorter. Without it the search
commits to a forty-tile detour round somebody who will have moved on in three
ticks, and it measures worse: `econ.spice/match` 81513 → 78076,
`result.points %` 100 → 91, `result.wipeouts %` 0 → 8.

The obvious companion to it — when the next tile is briefly occupied, wait a tick
instead of discarding the route — was built and **rejected on measurement**. It
costs `econ.spice/match` 81513 → 71786, `result.points %` 100 → 83 and
`result.wipeouts %` 0 → 16, and buys only `turret.entries/match` 24 → 9. The
note in `Script_Unit_CalculateRoute()` records it so it is not tried a third
time.

`Script_Unit_HasRoute()` uses the same search, and the tactical ring loops in
[unit.c](src/unit.c) route their whole candidate set in **one** search rather
than one per tile (`Unit_AttackPosition_RouteTicks()`) — a settled tile's cost is
final, so the answers are identical.

`--pathfinder-self-test` is the guard. It checks admissibility against reference
distances, that the route is connected and passable, that it costs exactly what
an independent relaxation says the shortest route costs — for the built obstacle
and for 48 destinations sampled across the map — that the same question twice
gives the same bytes, and that the metric matches the engine by driving a unit
across a tile and counting ticks.

Measured, `--pathfinder=0` against `1`: `econ.spice/match` 58339 → 81513,
`result.points %` 66 → 100, `result.wipeouts %` 25 → 0, `harv.lost.early` 8 → 0,
`wave.matches %` 66 → 100, and the suite goes from FAIL (1 regression) to PASS.
It costs `turret.entries/match` 10 → 24 — **on its gate of 24**, which is the one
number this change leaves without margin — and `turret.dwell/match` 3752 → 7148.
`--economy-baseline` is 11262/4982/1729 → 10701/4730/3481. At equal simulation
work the search costs nothing measurable (`--mp-replay=40000,500` is 7.2 s
either way); `--war-metrics` takes 16 s → 65 s because the matches now last
longer and neither army gets wiped out.

## Concrete on sand

`build_slab_on_sand` (default 1) lets a slab be laid on sand, dune and spice as
well as rock — `Structure_SlabAllowedOn()` in
[structure.c](src/structure.c). Nothing else moves: `isValidForStructure` still
refuses a *building* on bare sand, so paving is a purchase and not a decoration,
and walls are left out of the rule on purpose.

The "road" half needed no code at all. `g_table_landscapeInfo[LST_CONCRETE_SLAB]`
already carries `movementSpeed` 255 for every movement type against sand's 112
([landscapeinfo.c:129](src/table/landscapeinfo.c:129)) — the fastest surface in
the game, with nowhere to use it. In real time that is a Tank crossing concrete
in 51 ticks against 78 on sand and a Soldier in 51 against 111, so a pathfinder
that measures time prefers it without being told to.

**A warning about `Unit_GetTileEnterScore()`, because the obvious reading of it
is wrong.** It inverts speed into a cost with `res ^= 0xFF`, and read on its own
that says concrete costs 0 and sand 143. That only holds with
`dune2_enhanced=0`. On the default the rates are first scaled by
`movingSpeedFactor/256` — 25/256 for a Tank — so the real numbers are 231
against 245, a difference of six percent rather than of everything. The routing
metric is in [pathfinder.c](src/pathfinder.c) for that reason; see
"Route finding".

The skirmish AI is unaffected: `Skirmish_LaySlab()` writes the ground tile
directly and never asked this rule in the first place.

`--build-rules-self-test` checks both halves against a generated map.

## Repair all

`Structure_AutoRepair_*` in [structure.c](src/structure.c). A switch on the
Repair facility's panel that puts every damaged building of the House into
repair, one `Structure_AutoRepair_Consider()` call per structure per structure
tick, which sets exactly the flag the building's own Repair button sets and
leaves the rest — the credits, the five hitpoints a tick, the stop when the
money runs out — to the original game's code in `GameLoop_Structure()`.

Four decisions, each of which is a line of code and a reason:

* **The state is the House's, not the building's**, and it lives in a spare
  `HouseFlags` bit — the one `unused_0020:3` was holding. `SLDT_HOUSEFLAGS`
  folds it as 0x20, so the ODHO chunk is the length it was and a savegame
  written before this loads with the switch off. Several Repair facilities
  therefore show one setting rather than one each.
* **It travels as `MP_CMD_AUTO_REPAIR`**, a toggle rather than a state, for the
  same reason `MP_CMD_STRUCTURE_REPAIR` is one: it spends credits and repairs
  buildings, so it happens on both machines or the match is over. The flag is
  inside the house chunk's checksum, which is what would catch it if it ever
  did not.
* **It does not set `onHold`** the way the button does. The tick already refuses
  to produce while `repairing` is set, so the flag buys nothing here — and it is
  *not* cleared when the credits run out, which would leave a factory held for
  ever by a repair that never started.
* **A factory with something inside it is left alone** (`countDown != 0 &&
  linkedID != 0xFF`, plus the Construction Yard's own countdown). Repairing
  stops production; stopping every factory in the base is not what somebody who
  asked for their turrets to survive has asked for. The building's own button
  still does that.

The label is `STR_AUTO_REPAIR`, one of the strings the fork adds — the original
data files have no entry for it, so `String_Get_ByIndex()` answers everything
from `STR_FORK_FIRST` (0x7100) out of `s_forkStrings` in
[string.c](src/string.c). 0x7000 is not in that range because the main menu's
lobby row took the number first and resolves it before asking for a string.
The button is widget index 13 and it shares the Construction Yard's "place it"
row — the only free space on the panel — which is safe because one belongs to
the Construction Yard and the other to the Repair facility.

`--auto-repair-self-test` is the guard. It checks the rule one condition at a
time without a map, because the rule is entirely about flags: off does nothing,
on repairs a damaged turret, no Repair facility standing does nothing but keeps
the setting, full health / upgrading / rubble are left alone, a busy factory is
left alone and an idle one is not, and the switch toggles both ways. It also
measures the label in the 6p font the panel actually draws with and fails if it
does not fit the button — "Auto Repair" is 65 pixels against the 60 there are,
which is how the button came to say "Repair All".

## The tech tree

`tech_tree` (default `stock`) names which tree the game plays —
`Structure_TechTree_*` in [structure.c](src/structure.c), patched into
`g_table_structureInfo` at start-up next to the combat balance module.
`--tech-tree=stock|mp` overrides the key from the command line, for the same
reason `--pathfinder=` exists: the `opendune.ini` in `~/Library/Application
Support/` is searched first and shadows anything put in `bin/`.

**In a campaign a building is gated twice, in a match only once.**
`Structure_GetBuildable()` checks `g_campaignID >= availableCampaign - 1` *and*
the `structuresRequired` mask; `Skirmish_Prepare()` sets `g_campaignID` to 8,
past the latest building on the list, so in multiplayer the mask is the whole
tech tree. That is what makes it worth configuring at all.

`mp` is the fork's tree: the Refinery carries the early branches (Barracks,
Light Factory, Wall, Turret, Silo, Outpost), the Light Factory carries the Heavy
Factory and the Repair yard, the Outpost carries Hi-Tech, IX and Starport, the
Rocket Turret hangs off **IX** rather than off two Construction Yard upgrades,
and the Palace wants Hi-Tech, IX and Starport together. Two details that are
not arbitrary:

* **WOR keeps `refinery` beside `barracks`.** `Structure_GetBuildable()` waives
  the Barracks bit for Harkonnen — House identity, deliberately left alone — and
  a mask of `barracks` alone would leave them with *no* prerequisite, i.e.
  rocket infantry on the first tick.
* **The Construction Yard drops to one upgrade** (`upgradeCampaign[1] = 0`). Its
  second level only ever unlocked the Rocket Turret; left standing it is 200
  credits and twenty ticks for nothing. Level 1 stays — the large slab needs it.

Per-key overrides ride on top of the named tree: `tech_req_<building>` (a list,
`none` for nothing), `tech_upgrade_<building>` (Construction Yard level) and
`tech_upgrade_levels_<building>` (which can only remove levels). Names are the
table name lowercased with non-letters collapsed, plus readable aliases —
`light_factory` for `light_fctry`, `house_of_ix` for `ix`, `radar` for
`outpost`. `bin/opendune.ini.sample` lists both trees side by side.

**A tree the players disagree about cannot start a match**, and that is free:
`Lobby_FoldStructure()` already folds `structuresRequired`,
`upgradeLevelRequired` and `upgradeCampaign`, so the tree is inside the room
name. Measured: `x.0.713e1bf9` on stock against `x.0.20d7121b` on `mp`. This is
also why the module patches the table at **start-up** and not at match start —
the digest is taken in the menu, before either player has clicked BEGIN.

`Structure_TechTree_Validate()` refuses a tree with a cycle, with a building
nothing can reach, or with an upgrade level the Construction Yard does not
offer, and keeps stock instead. `--tech-tree-self-test` is the guard: it checks
that `stock` is byte-for-byte the compiled table (a module that quietly plays a
game of its own on machines that set no key is the failure worth catching), that
the `mp` preset is what this file says it is, that the validator refuses both
bad shapes, and then **plays it** — the real `Structure_GetBuildable()` on a
real Construction Yard, growing `structuresBuilt` one building at a time, at
campaign 8 with the house human-controlled. Ordos on purpose: the Harkonnen WOR
waiver and the Atreides WOR ban are House identity and do not belong in a test
of the tree.

The AI is waived from the tree by `Structure_GetBuildable()` but *not* by
`Skirmish_Plan_PickNext()`, which checks the mask itself — so a changed tree
reorders the AI's base rather than stalling it. Measured under `mp`, the two
Rocket Turrets the blueprint lists before the House of IX are simply built after
it.

## The build queue

A factory takes repeat orders — left click on its picture in the action panel is
one more, right click is one fewer, and the corner shows how many. The
Construction Yard now takes them the same way, and everything that is different
about it comes from one engine fact: **`Structure.linkedID` is a single slot.**
A factory empties it by driving the unit out of the bay; a yard cannot, because
the player has to find a spot first, so the original game simply stopped after
one.

`Structure_Queue_*` in [structure.c](src/structure.c) is the whole of it.

* **The finished building goes nowhere.** `Structure_Queue_Stash()` frees the
  completed `Structure` and remembers it as a count and a type, which vacates
  `linkedID` so the next order can start. It is created again at the moment it
  is placed (`Structure_Queue_PlaceReady()`), and freed again if the spot is
  refused, so a refusal costs nothing and the pool ends where it began. Keeping
  N of them allocated instead would spend N slots of the structure pool on
  buildings that are not on the map — and would have to go into the savegame,
  which has no room for it.
* **One stack, one type.** Choosing a different building refunds the whole stack
  (`Structure_Queue_RefundReady()` beside the `Structure_CancelBuild()` that was
  already there). That is the same bargain the original game struck with the one
  finished building it could be holding, and it is what lets the stack be a
  count and a type rather than a list.
* **The panel reads `N/M`** on the bottom row of the production widget — ready to
  place, and everything still owed including what is on the bench. The clicks
  move M. The corner where a factory writes `x3` is where a yard draws the
  footprint grid of what it is making, which is why the count moved.
* **"Place it" became a button of its own**, widget index 12, on the one free row
  of the action panel (y=123; the panel ends at the radar, y=136). The picture
  above it counts orders now, so it could not also start placement. The button
  is there only while the yard is holding something finished.
* **Placement stays open.** Four finished slabs go down in four clicks —
  `GUI_Widget_Viewport_Click()` leaves `SELECTIONTYPE_PLACE` only when the last
  is spoken for.

That last point is the one that needed care in a match. The placement command is
stamped for a later turn, so between the click and the turn that runs it the
yard still says it is holding four. `Structure_Queue_GetPlaceableCount()` is the
live count **less what has already been clicked for**
(`Structure_Queue_PlaceCommit()` / `..._PlaceRelease()`), and that counter is
local presentation state: it decides only whether the cursor stays in placement
mode, is never read by the simulation, and on the other client simply stays at
zero. Committing before `MpCommand_Submit()` rather than after is deliberate —
outside a match a submitted command runs immediately.

`g_structureActive` now points at the **yard** rather than at the building. With
a stack there is no single `Structure` to point at, and the only thing anything
ever read off that pointer was the house.

**A slab takes a quarter of the time of a large slab**, which it did not.
Westwood gave `Concrete` and `Concrete4` a `buildTime` of 16 apiece against
build credits of 5 and 20 — invisible in a campaign where a slab is laid once,
and plainly wrong in a match where concrete is a road ("Concrete on sand").
`Concrete` is 4 now. It is folded into the lobby digest like every other table
value, so two players are on the same number or they never meet.

`--build-queue-self-test` is the guard, and it plays rather than asserts: a real
match, the house's own yard, and the buildings finished by letting the clock run.
Three orders must all finish (without the stash it waits out the limit at one),
all three must go down on legal ground, the refunds must be the price to the
credit, choosing another building must empty the stack, and a clicked spot must
be a spent spot. Verified to fail: six deliberate breaks — stash disabled,
refund withheld, `buildTime` back to 16, the commit counter stubbed, the
type-change refund removed, and placement not taking one off the stack — produce
six different named failures.

The AI is untouched: it lays concrete straight into the map (`Skirmish_LaySlab()`)
and never asks the yard, and `Structure_Queue_CanOrder()` requires
`Match_IsHumanControlled()`. Measured — `--war-metrics` on and off the slab
change is **bit-identical across all sixteen numbers**.

## Skirmish — the AI test bench

`./opendune --skirmish` (optionally `--skirmish=ordos,harkonnen`) generates a
62x62 map, puts two AI houses on it and lets the human watch. Each AI starts with
a Construction Yard and a *plan* — the rest of its base as an ordered list of
(type, position) it has to work through itself. It is how the AI is developed and
judged in this fork, and the two AIs do fight: the default strategy spends nothing
on an army until t35000 and most of its income after. There is deliberately no menu
entry: the main menu list is sized by its first `STR_NULL`, so a sixth item makes
the whole menu vanish on a profile that has both a savegame and a Hall of Fame.

→ [skirmish.md](skirmish.md) — the spectator model, the plan hooks, and the
engine facts (player-centric AI checks, the credit clamp, the truncated
`buildable` mask) you need before touching it.

`--economy-search` evolves the fastest economic opening in that same sandbox,
with combat switched off, by simulating thousands of matches headless.
→ [economy.md](economy.md).

`--war-matrix` / `--war-timing` put two tuned economies on one map and vary how
much of the take each spends on its army. Fitness is not spice but the outcome,
so the answer is a win matrix rather than a number. → [war.md](war.md).

`--war-telemetry` records one match to `bin/telemetry/` as CSV, and
`python3 tools/telemetry_report.py` bakes every recording into `telemetry.html`
— a match picker with a chart. It has to bake rather than read the folder at view
time, because a page opened from the filesystem may do neither.
→ [telemetry.md](telemetry.md).

## The unit pool is partitioned by type, and it is easy to hit

There is one global pool of unit slots, and `Unit_Allocate()` does not take any
free one: it scans only the band assigned to that unit *type*
(`indexStart` / `indexEnd` in `g_table_unitInfo`) and fails when the band is
full. `House.unitCountMax` is a second, weaker limit on top of it.

Two consequences worth knowing before touching anything that scales the game up:
**every ground unit of every house shares one band** — harvesters included — and
**projectiles are units too**, with their own small band, so a bigger army does
not deal proportionally more damage unless that band grows with it. This fork
raised `UNIT_INDEX_MAX` to 242 and re-cut the bands; the layout, the savegame
implications and the 254 ceiling are in [units.md](units.md).

## Verifying a change

There is no test suite. These run headless and exit:

```bash
cd bin
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --combat-balance-self-test
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --skirmish=ordos,harkonnen --build-rules-self-test
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --skirmish=ordos,harkonnen --build-queue-self-test
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --tech-tree-self-test
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --auto-repair-self-test
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --skirmish=ordos,harkonnen --move-rules-self-test
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --skirmish=ordos,harkonnen --pathfinder-self-test
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --lobby-self-test
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --selection-self-test
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --skirmish-self-test=200000
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --economy-baseline=80000,3
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --skirmish=ordos,harkonnen --doctrine=B,A --war-metrics=200000,6
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --skirmish=ordos,harkonnen --mp-checksum=20000,5000
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --skirmish=ordos,harkonnen --mp-replay=40000,500
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --skirmish=ordos,harkonnen --mp-modal=40000,4000
```

**`--war-metrics` is the guard on every behavioural change**, and it is the one to
run before and after touching `doctrine.c`, `skirmish.c` or the AI hooks in
`unit.c`. Six maps played twice with the doctrines swapped, about forty seconds,
sixteen named numbers with a regression gate and a goal beside each. It is
deterministic: the same binary gives the same table. → [metrics.md](metrics.md).

The selection test replays the saves in `~/Library/Application Support/OpenDUNE/`.
The skirmish test simulates a match as fast as the CPU allows and prints each
AI's build order — see [skirmish.md](skirmish.md) for how to read it. Its map is
random, so run it twice before calling a change a regression.
The economy baseline is the cheapest guard on the harvester layer: baseline 0
should refine around 8000. If it drops to near zero, harvesters have stalled —
add `--economy-trace` and read the per-harvester and per-refinery state it
prints, then [harvester.md](harvester.md). It repeats its build orders between
runs but not its spice totals: the 60 Hz ticker keeps advancing `g_timerGame`
alongside the loop, so the absolute clock differs run to run
([mp.md](mp.md), stage 0).

`--mp-modal=ticks[,step[,seed]]` plays the same match twice and opens the modal
screens in one of the passes — the world-facing half of the options screen, the
mentat and the build list, at a fixed cadence, with the event loops left out
because those are pumped already. Two identical sample sets mean a player may
read the manual without ending the match for the other one. Add `--sim-purity`
and the same run also names *which call* and *which chunk*, instead of leaving
the answer to a sample thousands of ticks later; `--mp-modal-dump=TICK` leaves
both passes' copy of one sample on disk for `tools/mpdesync_diff.py`. Run it on
several seeds: the first bug it found showed on one seed in three.

**Guards on match-shared state key on `Match_IsActive()`, not `MpTurn_IsActive()`.**
The turn loop starts several hundred milliseconds after the match is built, and
a repaint in that window is enough. `MpTurn_IsActive()` is right only for things
that are about the turn loop itself — the packet cadence, the opcode budget, the
buttons that must go dead while it runs. Three of the four bugs `--mp-modal`
found were this distinction.

`--mp-checksum=ticks[,step[,seed]]` is the determinism harness: it seeds both
generators from the map seed, takes the clock off the wall and prints a CRC of
the serialised state per savegame chunk. Two runs must print the same log. It is
the guard on anything that could make the simulation depend on wall time, host
state or pointer values — and the precondition for multiplayer.
`--mp-replay=ticks[,step[,seed]]` plays a match twice in one process: once with a
scripted player issuing commands, once replaying the recording. It is the guard
on the command layer and on anything that has to start a match from a known
state. Adding `--mp-record=FILE` or `--mp-play=FILE` splits those two passes
across two processes — one writes the recording and prints its checksums, the
other reads it and prints its own, and `diff` decides:

```bash
./opendune --skirmish=ordos,harkonnen --mp-replay=40000,500 --mp-record=rec.mpc > a.log
./opendune --skirmish=ordos,harkonnen --mp-replay=40000,500 --mp-play=rec.mpc   > b.log
diff <(grep '^mp-checksum' a.log) <(grep '^mp-checksum' b.log)
```

`--human=N` marks skirmish slot N as played by a person rather than the AI
(`--human=1,2` for both); under `--mp-replay` the scripted player takes that
house over, which is what puts the unit orders under test.
`--mp-viewpoint=ticks[,step[,seed]]` is the same two passes seen from the two
different houses, and it names every chunk the simulation still decides from
`g_playerHouseID` — the guard on §1 of [mp.md](mp.md). Run the replay and the
viewpoint harness on **several seeds**: both have passed on seed 1000 while
failing on others.

`--mp-turnloop=ticks[,step[,seed]]` plays the match through the lockstep turn
loop, where a command is stamped for turn N+D instead of happening now. On its
own it uses a loopback transport; `--mp-net=slot,dir` makes this process one
player of two and the directory their wire, so two processes play one match and
`diff` of their checksum logs decides. `--mp-turn=TL,D` sets turn length and
delay, `--mp-realtime` paces at the speed in force and `--mp-lag=ms` holds
packets back at the sender — together they measure whether a given ping stalls
anybody.

**D is derived from the speed, and the speed is shared.** The budget for a
packet to arrive is `D * TL` ticks, which is `D * TL / (60 * M)` seconds — so
doubling the speed halves it, and the default D at x4 leaves 50 ms against a
relay whose p90 is 170. `MpGame_DelayForSpeed()` in [opendune.c](src/opendune.c)
therefore uses `D = ceil(Dc * M / 2)`, holding the budget where `--mp-turn`
calibrated it. The speed itself travels as `MP_CMD_MATCH_SPEED`: two clients
with different delays stall each other, and the options screen's Normal/Fast row
is a simulation input (`Tools_AdjustToGameSpeed()` reads it for walking speed and
fire delays), so both halves change on both machines in the same turn.
`MpTurn_SetDelay()` is what makes a mid-match change safe in either direction —
raising D fills the gap with empty packets, lowering it waits for the clock
rather than re-addressing an outbox whose turn has already been sent.

```bash
./opendune --skirmish=ordos,harkonnen --human=1,2 --mp-turnloop=8000,500 --mp-net=1,/tmp/net &
./opendune --skirmish=ordos,harkonnen --human=1,2 --mp-turnloop=8000,500 --mp-net=2,/tmp/net &
```

`--mp-relay=host[:port],room[,slot]` is the same two processes over a real TCP
socket, through the relay in `tools/relay/` — the transport a match on the
internet actually uses. Start the relay first; both sides must name the same room
and the same seed and take different slots. `--mp-wait=ms` bounds both the wait
for the second player and the wait for a packet, and the relay's own `-lag`
emulates a ping (it belongs there, not in the game: a client must not be able to
tell a slow relay from a slow opponent).

```bash
tools/relay/relay -listen 127.0.0.1:31337 -verbose &
./opendune --skirmish=ordos,harkonnen --human=1,2 --mp-turnloop=20000,1000 --mp-relay=127.0.0.1:31337,r1,1 &
./opendune --skirmish=ordos,harkonnen --human=1,2 --mp-turnloop=20000,1000 --mp-relay=127.0.0.1:31337,r1,2 &
```

`--mp-relay` *without* `--mp-turnloop` is the real game: two windows, two
players, the map drawn and the simulation stepped by the turn loop underneath.
`tools/mpduel.sh` starts both sides on this machine and says at the end whether
they agreed. `--speed=N` sets the tick multiplier, `--mp-units=N` gives each
player a starting squad, `--mp-seed=N` names the map.

Every turn packet carries a checksum per savegame chunk, so a live match reports
its own desync — `mp-live: DESYNC at turn 6 (tick 48), about: map rng`. Add
`--mp-desync-dump` and both clients keep the last few turns on disk, so the turn
the mismatch names can be diffed byte for byte (`mpdesync-s*-turnN.bin`).
`python3 tools/mpdesync_diff.py <logdir>` reads that pair and says what the two
clients disagreed about in words — which tile, which animation slot — rather
than which byte. That pair — the chunk name and the two dumps — is how the
desyncs in stage 6 of mp.md were found, one run each.

→ [mp.md](mp.md), [tools/relay/README.md](tools/relay/README.md).
The same dummy-driver invocation without a flag is a useful smoke test that data
loads — it starts the real game, so kill it (`pkill -9 -f opendune`) rather than
leaving it running.

## The lobby — a match without a command line

**PLAY SOMEBODY** is the second row of the main menu ([gui/lobby.c](src/gui/lobby.c)).
It settles a relay address, a game code, which of the two players you are, which
pair of houses, and starts the same match `--mp-relay` starts — `MpGame_TakeLobbyChoice()`
in [opendune.c](src/opendune.c) fills in exactly the fields the flags would have.

The design is one sentence: **everything the two players must agree on is folded
into the room name**, `code.pair.confighash`. The relay puts two clients together
only if they ask for the same room, so a disagreement about the map, the houses,
the build or the balance ini cannot start a match — it shows up as *the other
player never joined*, which is a thing a person can act on. Letting them meet and
disagree would be a desync a few seconds later, which is not. There is therefore
no handshake packet: the room name is the handshake, and the relay enforces it
without knowing that it does.

* The **game code** is the only thing anybody has to exchange. The map seed is
  `crc32(code) | 1`, so both sides derive the same map from it and no seed is
  ever sent.
* The **config hash** is `crc32(revision + g_table_unitInfo + g_table_structureInfo)`.
  Hashing the tables rather than the ini file is what makes it exact: the balance
  module works by patching those tables, so a key written out at its default
  value, a comment or a blank line changes nothing, and a real difference always
  changes the hash.
* The **house row** cycles the six *ordered* pairs, and the player row swaps which
  end of the pair is yours. Both players see the same pair — it is in the room
  name — and pick opposite seats.
* The relay defaults to `mp_relay` from `opendune.ini`, then to the project's own
  relay. Nobody should have to type an address twice.

**The digest is folded from field values, and it may never be taken over
memory.** It was, and it was wrong twice for it. First it was a CRC over the
table bytes — which include `ObjectInfo`'s `name` and `wsa`, two addresses that
a position-independent binary puts somewhere different on every launch. Two
copies of one build asked for different rooms and each waited out its timeout
for somebody who was in the other one. Reaching round the pointers fixed that
and left the deeper mistake standing: the bytes *between* the fields belong to
the compiler, and an arm64 build and an x86_64 build of the same commit still
disagreed (measured: `74d90780` against `f2019082`, branch decoration already
removed). `Lobby_Fold()` now folds each field as four little-endian bytes, in an
order this file chooses; only the values are the same on both.

**The branch name is not part of the agreement either.** `g_opendune_revision`
is `g<sha>[M][-<branch>]`, and the branch is where the build happened rather
than what it is — the Intel package is built in a worktree, which is detached
and therefore has no branch at all. `Lobby_RevisionSpan()` cuts at the first
dash. The `M` stays: a modified tree really may be different code, so two dirty
trees agree only by being equally dirty, which is as much as anything can tell.

`--lobby-self-test` is the guard, and it tests the rule rather than the drawing:
same choices → identical room string, any difference → a different one, the seed
stable and never zero, `host` / `host:port` / `host:` / `host:0` all split the
way `MpNet_Connect()` needs, and — the check that was missing — **the digest does
not move when a pointer does**. Comparing two calls inside one process was what
let the pointer bug ship; the test now repoints a table entry's `name` at a copy
of the same string and demands the digest stay put, and separately that a real
balance change always move it.

`--lobby-play=relay,code,pair,slot` is the other half: it enters the menu as
though PLAY SOMEBODY had just been clicked and takes the BEGIN branch with those
values, so two headless processes play the lobby's own road end to end. It fires
once and reports `lobby-play: FAIL` rather than returning to a menu nobody is
sitting at.

```bash
./opendune --lobby-play=127.0.0.1:31337,dune42,0,1 &
./opendune --lobby-play=127.0.0.1:31337,dune42,0,2 &
```

It also prints the room name, which is the **only way to check the one claim no
single-process test can reach**: that two builds of the same commit for two
architectures agree. Run it against each binary and compare the string — both
must print the same `code.pair.digest`, or an Intel player and an Apple Silicon
player cannot meet. Do this every time both packages are made; it is the last
step of the Intel procedure above for that reason. Both trees must be at the
same commit and equally clean, since the `M` flag is inside the digest.

Two traps in the menu itself. `mainMenuStrings` is a **seven**-column table now,
one row per savegame/Hall-of-Fame combination, and every row has to carry the
lobby entry or it disappears on some profiles. And the menu list is still sized
by its first `STR_NULL`, which is why the lobby is an entry in that table and not
an extra item appended after it.

Two more things the first version got wrong, both about what happens when no
match starts. Joining used to be a `msleep()` loop — no SDL events pumped and no
frame drawn, so the window went black and macOS put a beachball over it for the
whole thirty seconds. It waits on `sleepIdle()` now, with a notice on screen,
ESC to give up, and the reason held up for six seconds when it ends badly. And
the menu's `stringID` was left on `STR_LOBBY_MENU` when the lobby handed off, so
a failed match reopened the lobby the instant it failed — over the black screen
GM_SKIRMISH had already cleared — and again thirty seconds later, for ever. It
lands back in the menu now, with the lobby's rows still filled in.

**Only the logic is verified.** `--lobby-self-test` and `--lobby-play` cover the
rule and the handoff, and the menu draws under the dummy video driver without
crashing, but no human has seen the lobby window or clicked a row.

## Conventions

* **ANSI C (C89), not C++.** Declarations at the top of a block, `/* */`
  comments, no `//`, tabs for indentation.
* Follow the surrounding style; the project's own guide is the
  [OpenDUNE Coding Style wiki page](https://github.com/OpenDUNE/OpenDUNE/wiki/Coding-Style).
* Comments explain *why*, especially where our code works around the original
  script. Upstream quirks that look like bugs are usually deliberate.
* Fork work is committed straight to `master`.

## Deeper notes

* [emc-scripts.md](emc-scripts.md) — script VM, disassembly, shared-state rules
* [mp.md](mp.md) — internet multiplayer (deterministic lockstep): the plan, and
  the engine changes it needs — `g_playerHouseID` as a simulation input, the
  shared RNG streams, per-house fog, the modal windows that stop the world, and
  the radar animation. Stages 0 to 6 are built — determinism, the RNG split, the
  match descriptor, the command layer, the turn loop, the relay and the real
  game loop with its desync detector; the lobby and the rest of the interface
  work are still design
* [harvester.md](harvester.md) — harvester state model and its bug history; the
  worked example of supplementing a script correctly
* [skirmish.md](skirmish.md) — AI vs AI mode: spectator model, base plans, and
  the AI's known weak spots
* [economy.md](economy.md) — the economy search: how a plan is encoded, what it
  found, and which drivers actually matter
* [war.md](war.md) — the economy/army split: the budget model, the win matrix,
  and why the timing of the split beats its level
* [metrics.md](metrics.md) — the regression suite: the sixteen numbers, their
  gates and goals, and what had to be fixed before any of them could be trusted
* [telemetry.md](telemetry.md) — recording one match: the CSV format, the report
  generator, and how to add a metric
* [mp-actions.html](mp-actions.html) — every action a player can take inside a
  networked match, whether it changes the world or only this machine, and which
  command carries it. The audit that replaced hunting desyncs one at a time
* [instruments.html](instruments.html) — every measuring surface in one page: the
  suite, all 58 CSV columns, the overlay, the one-off reporters and the search
  outputs, each with what it counts
* [units.md](units.md) / [units.html](units.html) — complete unit-type table, and
  the pool partition that caps how many units can exist at once
* `INTERNALS.txt` — palette and file-format notes from upstream
* `enhancement.txt` — upstream's list of deviations from the original game
