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
| `tools/` | asset extractors, `dis_emc.py` for the game scripts, `telemetry_report.py` for recorded matches, `relay/` (the multiplayer relay, Go) |
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

Its integration step fires a real Atreides Soldier shot (base 10) at a synthetic
Harkonnen Trooper (45 HP), so a strong enough `class_damage_p_vs_rp` makes that
shot lethal and reaches `Unit_SetAction(ACTION_DIE)` with `UNIT.EMC` not yet
loaded — `Sprites_LoadTiles()` only reads it when a scenario loads. The NULL
guard in `Script_Load()` ([src/script/script.c:283](src/script/script.c:283))
exists for exactly that; **rebuild before trusting a segfault here**, a stale
`bin/opendune` predating that guard crashes instead.

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
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --selection-self-test
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --skirmish-self-test=200000
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --economy-baseline=80000,3
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --skirmish=ordos,harkonnen --doctrine=B,A --war-metrics=200000,6
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --skirmish=ordos,harkonnen --mp-checksum=20000,5000
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --skirmish=ordos,harkonnen --mp-replay=40000,500
```

**The last one is the guard on every behavioural change**, and it is the one to
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
delay, `--mp-realtime` paces at 60 Hz and `--mp-lag=ms` holds packets back at the
sender — together they measure whether a given ping stalls anybody.

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
