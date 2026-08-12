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
| `src/` | engine: `unit.c`, `structure.c`, `map.c`, `house.c`, `opendune.c` (main loop) |
| `src/script/` | the EMC virtual machine and its opcode implementations |
| `src/gui/` | viewport, widgets, input, drawing |
| `src/table/` | static data: `unitinfo.c`, `structureinfo.c`, `actioninfo.c` |
| `src/saveload/` | save format — extending a struct means touching this |
| `tools/` | asset extractors, and `dis_emc.py` for the game scripts |
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
`OpenDUNE.app`. **No copy exists yet on this machine**, so the game currently
runs on the compiled-in defaults.

The module itself is `Unit_CombatBalance_*` in [src/unit.c:65](src/unit.c:65):

* `Unit_CombatBalance_Init()` — called once from `opendune.c`; reads every key,
  clamps percentages to 0..1000, and patches `g_table_unitInfo` /
  `g_table_structureInfo` (infantry range, shared Barracks production). The
  compiled-in defaults are the `s_combatBalance` initialiser — they are the
  fallback for every missing key, so **defaults live in two places** and the
  sample ini must be kept in step with them.
* `Unit_CombatBalance_ApplyHouseDamage()` — House identity bonus, applied to the
  base shot in [src/script/unit.c:664](src/script/unit.c:664), i.e. it also
  affects damage to structures.
* `Unit_CombatBalance_ApplyClassDamage()` — the 4x4 attacker/target matrix,
  applied at impact in [src/map.c:435](src/map.c:435), unit-versus-unit only.

Classes are P (Soldier, Infantry), RP (Trooper, Troopers), LT (Trike, Raider
Trike, Quad) and TT (Tank, Siege Tank, Devastator); every other unit is neutral
(x1.00) on both sides of the matrix. Values are integer percentages — 100 is
x1.00. Keys: `class_balance_enabled`, `class_balance_shared_infantry`,
`class_range_p_bonus`, `class_damage_<attacker>_vs_<target>` for the 16 matrix
cells, and `class_bonus_atreides_p` / `class_bonus_harkonnen_rp` /
`class_bonus_ordos_trike` (the Ordos bonus deliberately skips Quad).

Changing any default means touching three files: the `s_combatBalance`
initialiser, `bin/opendune.ini.sample`, and the README.txt section. Verify with
`--combat-balance-self-test` below, which checks the matrix, the neutral classes,
the House bonuses and the shared-Barracks patch against the loaded config.

**Known limit of that self-test:** its integration step fires a real Atreides
Soldier shot (base 10) at a synthetic Harkonnen Trooper. If the configured
multipliers make that shot lethal — `class_damage_p_vs_rp` at 400 or above — the
Trooper dies, `Unit_SetAction(ACTION_DIE)` runs, and `Script_Load()` segfaults:
`UNIT.EMC` is only loaded by `Sprites_LoadTiles()` when a scenario loads, which
never happens on this path, so `g_scriptUnit->start` is still NULL. That is a
harness limitation, not a game bug — use `--selection-self-test` (it replays real
saves, so the script pool is populated) to sanity-check such configurations.

## Verifying a change

There is no test suite. Two built-in self-tests run headless and exit:

```bash
cd bin
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --combat-balance-self-test
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --selection-self-test
```

The selection test replays the saves in `~/Library/Application Support/OpenDUNE/`.
The same dummy-driver invocation without a flag is a useful smoke test that data
loads — it starts the real game, so kill it (`pkill -9 -f opendune`) rather than
leaving it running.

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
* [harvester.md](harvester.md) — harvester state model and its bug history; the
  worked example of supplementing a script correctly
* [units.md](units.md) / [units.html](units.html) — complete unit-type table
* `INTERNALS.txt` — palette and file-format notes from upstream
* `enhancement.txt` — upstream's list of deviations from the original game
