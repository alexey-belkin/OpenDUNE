# Multiplayer — deterministic lockstep over the internet

**Status: stages 0 to 3 (plus 3a and 3b) are in the tree, the rest is design.** The determinism
harness, the RNG split, the match descriptor and the command layer exist as code,
and the sections at the bottom record what each cost and what each found. The
rest of this file is the plan and, more importantly, the list of things in the
engine that have to change before any of it can work. The order of the sections
is roughly the order of the work.

The target is player against player over the internet, with AI houses added
afterwards as a special case of the same machinery.

## Why lockstep

Every client runs the whole simulation; only *orders* travel over the wire. Turn
N is not executed by anybody until every player's packet for turn N has arrived.

Three facts about this codebase decide it:

1. **The simulation has no floating point.** `float`, `double`, `sqrt`, `sin`,
   `cos`, `atan` do not appear anywhere in `unit.c`, `structure.c`, `map.c`,
   `house.c`, `tile.c`, `team.c` or `src/script/*.c`. Integer-only arithmetic is
   the precondition for lockstep and it already holds.
2. **Determinism is already exercised.** `--war-metrics` prints the same sixteen
   numbers for the same binary ([metrics.md](metrics.md)), `--economy-baseline`
   is deterministic ([economy.md](economy.md)). The desync test bench exists
   before the network does.
3. **The whole match state is about 25 KB** — real saves in
   `~/Library/Application Support/OpenDUNE/` are 13–25 KB. A state checksum is
   therefore free, and "send the entire state" is a viable reconnect mechanism.

The alternative — an authoritative server shipping state — means serialising
units, structures, tiles **and the EMC script state of every object**
([src/saveload/scriptengine.c](src/saveload/scriptengine.c)). Script state *is*
game state and it is Westwood's ([emc-scripts.md](emc-scripts.md)); a delta
protocol over it is a fight with somebody else's bytecode. Lockstep never touches
it.

Rollback (GGPO-style) needs full save/restore every frame. 25 KB makes that
technically possible, but it buys input latency that an RTS with 130 ms turns does
not need.

## The turn model

`Timer_Tick()` is registered at 60 Hz ([opendune.c:1596](src/opendune.c:1596)) and
increments `g_timerGame` ([timer.c:361](src/timer.c:361)), so one simulation tick
is 16.67 ms.

| Symbol | Meaning | Default |
|---|---|---|
| `TL` | turn length in ticks | 8 ticks = 133 ms |
| `D` | turn delay: an order issued during turn N executes at turn N+D | 2, adaptive |
| `L` | one-way delivery A → relay → B | ≈ ping / 2 |

Per turn each client sends one packet, always, even empty — an empty command list
is the "I am ready" acknowledgement:

```
{ turn: u32, commands: [MpCommand], checksum: u32 /* state as of turn-2 */ }
```

Ordering inside a turn is fixed by rule, never by arrival: sort by
`(playerSlot, localSequence)` before applying. Two clients that received the same
packets in different orders must still apply them in the same order.

Topology is a **relay**, not peer-to-peer: direct P2P over the internet needs hole
punching and still fails on symmetric NAT. The relay is a few hundred lines, works
always, and gives lobby and room codes for free. Bandwidth is 7.5 packets/s of
maybe 30 bytes — under 300 B/s per client.

## What latency does, and what it does not do

This is the question worth being precise about, because the intuition "lockstep
means the game stutters" is half right and the half that is wrong matters.

**A late order can never cause a divergence.** Turn N is not executed until every
packet for turn N is present. There is no "apply what arrived and patch it up
later" path, because there is no patching mechanism at all. Late means *everyone
waits*. Correctness does not depend on timing in any way; timing only decides
comfort. This is the whole reason to choose lockstep.

The condition for a stall-free game is one inequality:

```
L < D × TL
```

The packet carrying orders for turn N+D is sent at the start of turn N, and is
needed D turns later. `D × TL` is its travel budget.

With `TL` = 133 ms, and taking ping as the round trip between the two players
through the relay:

| ping | L | D needed | budget `D × TL` | margin | order latency felt |
|---|---|---|---|---|---|
| 25 ms | 13 ms | 1 | 133 ms | 10× | 133–266 ms |
| 100 ms | 50 ms | 1–2 | 133–267 ms | 2.7–5× | 133–400 ms |
| 200 ms | 100 ms | 2 | 267 ms | 2.7× | 267–400 ms |
| 1000 ms | 500 ms | 5 | 667 ms | 1.3× | 667–800 ms |

"Order latency felt" is the gap between the click and the unit reacting: the
remainder of the current turn plus D turns, so `D × TL` to `(D+1) × TL`.

Now the failure mode, so it is on the record. **1000 ms ping with `D` left at 2:**
budget 267 ms, delivery 500 ms. Every turn the client runs its 8 ticks — 133 ms of
game time — then sits idle for 233 ms waiting for the packet. Duty cycle 36%: the
match runs at roughly a third of normal speed *in visible steps*. That is exactly
the stutter to be afraid of, and the cure is not luck, it is `D`.

So:

* **Adaptive `D`.** Measure the relay round trip continuously,
  `D = ceil((L + jitter_p95 + 20 ms) / TL)`, renegotiate between turns. Raise it
  fast, lower it slowly (hysteresis) — flapping `D` is worse than a `D` one step
  too high. Raising `D` costs *nothing* in smoothness; it only delays orders.
  This is the entire answer to "will the game run in jerks".
* **Render is decoupled from the simulation.** Draw at 60 fps regardless of the
  turn loop. Camera, selection, control groups and the sidebar keep working while
  the simulation is blocked, so a stall reads as lag rather than as a freeze.
* **Spread a turn's ticks across its wall-clock duration** instead of bursting all
  8 at the boundary. Under a healthy `D` the packet arrives a full D turns early,
  so there is always lead to spread into, and motion stays continuous.
* Prefer small `TL` and larger `D` over the reverse. `TL` sets the granularity of
  a stall; `D` only sets order lag. `TL` = 4 ticks (67 ms) is also reasonable, at
  15 packets/s.

Verdict per ping band: 25 and 100 ms are indistinguishable from single player.
200 ms is fine. 1000 ms is playable but sluggish, and Dune II tolerates it better
than most RTS games — orders are script-driven and already take tens of ticks to
visibly land, and there is no micro to speak of.

## No player is the source of truth

There is no host whose state wins. Lockstep has nothing to reconcile: either every
client agrees, or the match is broken. The relay's authority covers exactly two
things — the ordering of commands and the boundaries of turns — and that is all
the authority needed to keep the simulations identical.

In particular, with two players a checksum mismatch is **unattributable**: there
is no majority, so nothing identifies which side diverged. The correct response is
therefore:

* **Halt the match immediately.** Both clients dump their serialised state and
  report a desync.
* Do **not** adopt one side's state silently. If the wrong side wins you have
  corrupted the other player's game, and either way the bug is now invisible.
* Diff the two dumps. `src/saveload/*.c` is field-tabled, so the answer comes back
  as the name of the field that diverged, not as an offset into 25 KB.

Two related mechanisms are *not* this one and should not be confused with it:

* **Reconnect** — a player dropped, nobody diverged. This does need a designated
  state donor, chosen by convention (the room creator). 25 KB uploaded, loaded on
  the other side, resume at an agreed turn.
* **A headless authoritative simulation on the server**, later. The engine already
  runs headless — `--skirmish-self-test` drives the loop by hand with dummy
  drivers ([opendune.c:1206](src/opendune.c:1206)) — so the relay could run a third
  copy. That turns two-player desync into a majority vote and doubles as an
  anti-cheat oracle.

One honest limitation of lockstep: it cannot hide information from a modified
client. Every client simulates the whole world, so fog of war is advisory. This is
inherent, not a bug to be fixed later.

## What must be byte-identical between clients

Anything that changes the simulation and is currently a *local preference* becomes
part of the match configuration, exchanged and hashed at the handshake:

| Thing | Where | Why it desyncs |
|---|---|---|
| combat balance ini | `Unit_CombatBalance_Init()`, [unit.c:65](src/unit.c:65) | patches `g_table_unitInfo` / `g_table_structureInfo` at startup; different `opendune.ini`, different damage |
| `g_dune2_enhanced` | [opendune.c:1802](src/opendune.c:1802) | read from ini, and it changes game logic ([house.c:319](src/house.c:319), [unit.c:4322](src/unit.c:4322)) |
| `g_gameConfig.gameSpeed` | `OPTIONS.CFG`, [config.c:122](src/config.c:122) | multiplies the number of simulation passes per frame ([opendune.c:942](src/opendune.c:942)) |
| game data | `bin/data/*.PAK` | the EMC bytecode is the game logic |
| binary revision | `src/rev.c` | two builds are two games |

The handshake compares hashes and refuses to start on mismatch. Refusing to start
is cheap; a desync twenty minutes in is not.

## The engine work

### 1. `g_playerHouseID` is a simulation input, not a viewpoint

**Done — see "Stage 2" below.** 209 references, and in the game logic they changed
*behaviour*, not just drawing:

| Site | What differs |
|---|---|
| [house.c:377](src/house.c:377) | `House_AreAllied()`: everyone who is not the player is an ally by definition |
| [unit.c:3153](src/unit.c:3153) | a new unit gets `Unit_GetDefaultAction()` for the player, `ui->actionAI` for anyone else |
| [unit.c:3783](src/unit.c:3783), [unit.c:3797](src/unit.c:3797) | guard position is only set for the player's units |
| [unit.c:4538](src/unit.c:4538) | `ACTION_AMBUSH` is skipped for the player's house |
| [structure.c:1204](src/structure.c:1204) | credit refund differs |
| [structure.c:2095](src/structure.c:2095) | prerequisite checks only apply to the player |

A PvP match has two players, so a global scalar cannot decide these. Split into:

* `g_viewHouseID` — UI, fog, sound, minimap, and nothing else;
* an explicit *is this house human-controlled* bitmask for the places where the
  logic legitimately differs;
* the alliance matrix as **data**, not as "everyone except the player".

The trail is already half cut: `Skirmish_IsActive()` sits as a patch in exactly
these places ([house.c:375](src/house.c:375),
[structure.c:699](src/structure.c:699), [unit.c:2321](src/unit.c:2321)). Rather
than adding a third patch, introduce a match descriptor — a slot per house with
a controller (local human / remote human / AI / none) — and **make skirmish a
configuration of it**. That also retires the player-centric warnings in
[skirmish.md](skirmish.md).

**Scope for v1, decided rather than discovered:** two slots, no more. A match is
networked only when *both* slots are human; every other combination — human
against AI, AI against AI — runs entirely locally with no lockstep, no relay and
no turn loop. Mixed matches over the network (a human, a remote human and an AI
house) are a v2 question. The descriptor still carries the controller per slot,
because that is what the simulation asks about; what v1 forecloses is only the
number of slots and the combination that goes on the wire.

Two slots also make alliances trivial: in a match the two houses are enemies and
that is the whole rule. The alliance matrix as data waits for v2, when there can
be more than two of anything.

Guard: the sixteen numbers of `--war-metrics` must not move. This is the first
stage where that gate stops being a formality — every conversion here is a chance
to change AI behaviour by accident.

### 2. Fog of war is single-player

`Tile.isUnveiled` is **one bit per tile** ([map.h:35](src/map.h:35)) — fog exists
for exactly one house. Object visibility is already per-house (`seenByHouses`,
8 bits).

* **v1: no fog.** Everything revealed. Cheap and honest — skirmish already does
  `s->o.seenByHouses |= 0xFF` ([structure.c:699](src/structure.c:699)).
* **v2: `uint8 unveiledByHouses` per tile** — 4096 bytes total, plus a saveload
  field.

Fog is its own stage, not a prerequisite.

### 3. The RNG streams are shared between simulation and presentation

**Done — see "Stage 1" below for what it turned into.** This was the subtlest
desync source in the codebase, and it would have bitten in the first seconds. `Tools_RandomLCG_Range()` is consumed by:

* the simulation — the EMC `Random` opcode
  ([script/general.c:132](src/script/general.c:132)), map generation
  ([skirmish.c:1933](src/skirmish.c:1933));
* **and by presentation** — `Music_Play(Tools_RandomLCG_Range(0, 8) + 8)` at
  [opendune.c:1358](src/opendune.c:1358), [1383](src/opendune.c:1383),
  [1399](src/opendune.c:1399), [1406](src/opendune.c:1406); mentat animation
  throughout [gui/mentat.c:590-775](src/gui/mentat.c:590); screen fades at
  [gui/gui.c:3754](src/gui/gui.c:3754); sound choice at
  [gui/viewport.c:487](src/gui/viewport.c:487) (that one draws on
  `Tools_Random_256`, the other stream).

Two clients whose music happens to be in a different state pull different numbers
out of the shared sequence and diverge immediately.

Split into `Tools_Random*` — the simulation stream, seeded from the match seed and
carried in the save — and a new `Tools_RandomUI*`, free-running and unsynchronised.
Then audit `explosion.c` and `animation.c`: anything scheduled off `g_timerGUI`
([explosion.c:233](src/explosion.c:233), [animation.c:94](src/animation.c:94))
belongs to the UI stream, while anything the simulation causes stays on the
simulation stream.

### 4. There is no discrete simulation tick

`g_timerGame` is advanced by the timer thread ([timer.c:361](src/timer.c:361)) and
the subsystems schedule themselves against it from inside the GUI loop
([opendune.c:1533](src/opendune.c:1533)). Under lockstep the simulation must
advance only when the network permits.

The mechanism already exists: `--skirmish-self-test` drives exactly this by hand
([opendune.c:1206](src/opendune.c:1206)) —

```c
Timer_AdvanceGame();
GameLoop_Team(); GameLoop_Unit(); GameLoop_Structure(); GameLoop_House();
```

So in a multiplayer match: `Timer_SetTimer(TIMER_GAME, false)` for good, and that
step is called from the network-gated stepper instead. Two invariants:

* the stepper is called from **exactly one place** — never from a widget callback,
  never reentrantly, or command ordering breaks;
* `GameLoop_StepSpeed()` (the `[` and `]` keys,
  [opendune.c:946](src/opendune.c:946)) is disabled. Simulation rate is the
  network's business, not a per-client key.

### 5. Modal UI stops the world, and in multiplayer it must not

In the campaign, opening the Construction Yard build screen freezes the match.
That is not a side effect, it is deliberate: `Timer_SetTimer(TIMER_GAME, false)`
before the window and `true` after
([structure.c:1750](src/structure.c:1750)). The same pattern guards the options
screen ([widget_click.c:784](src/gui/widget_click.c:784)) and the mentat
([gui/mentat.c:379](src/gui/mentat.c:379)). `GUI_ChangeSelectionType()` also stops
it ([gui/gui.c:2286](src/gui/gui.c:2286)) and only restarts it in some branches —
`SELECTIONTYPE_MENTAT` deliberately leaves the simulation frozen
([gui/gui.c:2351](src/gui/gui.c:2351)).

In PvP this is unacceptable: one player reading a build list must not stop the
other player's harvesters. But note what is *not* an obstacle:

* **The timer call.** In multiplayer the wall-clock game timer is off anyway and
  the stepper owns the clock, so `Timer_SetTimer(TIMER_GAME, false)` becomes a
  no-op that only has to be prevented from gating the stepper.
* **`Sprites_UnloadTiles()`** ([structure.c:1744](src/structure.c:1744), reloaded
  at [structure.c:1756](src/structure.c:1756)). The factory window frees the map
  tiles for its own graphics, but the window is fullscreen — the map is hidden
  regardless. Losing the tiles costs nothing that the window has not already cost.

So there is exactly **one** obstacle: the nested event loop.
`GUI_DisplayFactoryWindow()` spins its own `for (...; sleepIdle())`
([gui/gui.c:2905](src/gui/gui.c:2905)) and never calls `GameLoop_*()`. Same shape
in `GUI_Widget_Options_Click()`
([widget_click.c:793](src/gui/widget_click.c:793)) and the invoice screen
([gui/gui.c:2912](src/gui/gui.c:2912)).

That makes the cheap path viable for a first version:

* **v1 — pump, don't rewrite.** Every nested loop calls `Mp_Pump()` in the
  `sleepIdle()` position: step the simulation, drain the socket, draw nothing. The
  fullscreen window stays exactly as it is; the match keeps running behind it. The
  player is blind for a few seconds, which is a fair cost for a build decision and
  is *not* a pause. One thing to verify: stepping the simulation while the tiles
  are unloaded must neither draw nor crash — the game loop itself does not draw
  (`GUI_DrawScreen()` is a separate call at
  [opendune.c:1546](src/opendune.c:1546)), but explosions and animations created
  during those ticks touch the animation tables and want checking.
* **v2 — non-modal build panel** drawn over the viewport: no tile unload, no
  nested loop, reads state and emits a command. Then the player is not blind
  either. The fullscreen window stays for the campaign.

Either way nothing may block the turn loop, ever.
* **Pause becomes a networked command**, so a unilateral local pause is impossible
  by construction. Whether one player may pause at all is a rules decision;
  a vote or a per-player quota is the usual answer.
* Regression test for exactly this: a scripted multiplayer session that opens each
  modal surface for N ticks and asserts the tick counter advanced and both
  checksums still match.

### 6. The radar animation blocks, and the radar rule needs rewriting

Switching the radar on or off hangs the game for a couple of seconds.
`House_UpdateRadarState()` ([house.c:385](src/house.c:385)) waits for any voice
to finish (`while (Driver_Voice_IsPlaying()) sleepIdle();`), plays one, and then
walks every frame of `STATIC.WSA` with `Timer_Sleep(3)` between frames — three
ticks at 60 Hz, so 50 ms per frame ([house.c:429](src/house.c:429)).

It is called from the simulation — [structure.c:799](src/structure.c:799) and
[structure.c:1547](src/structure.c:1547) — so a *game logic* call performs a
multi-second blocking animation. That is the same disease as §5 and it needs the
same cure, but there is a second problem stacked on it:

```c
if (h == NULL || h->index != g_playerHouseID) return false;
```

The flag is only ever maintained for one house. `House.flags.radarActivated` is
already per-house and already saved ([saveload.c:156](src/saveload/saveload.c:156)),
and it is read in exactly one place that matters — the minimap at
[viewport.c:1218](src/gui/viewport.c:1218). It never feeds the simulation. So
maintaining it for **every** house is free, deterministic, and required: another
instance of problem 1.

The split:

* **State** — a pure flag update, run for every house, no I/O, no sleeping, no
  RNG. Outpost present and power sufficient, that is all.
* **Presentation** — reacts to the flag of the *viewing* house on the next redraw.
  In multiplayer that is an instant switch: no WSA, no voice wait, no
  `Timer_Sleep`. The campaign keeps the animation, gated on the match descriptor
  exactly like the build screen.

### The minimap rule for multiplayer

`GUI_Widget_Viewport_DrawTile()` ([viewport.c:1201](src/gui/viewport.c:1201))
branches on `g_map[packed].isUnveiled && g_playerHouse->flags.radarActivated`:

| | today |
|---|---|
| radar on | terrain colour, plus any unit standing on the tile |
| radar off | only the player's own **structures** get a dot; everything else is colour 12 |

Wanted in multiplayer, with v1 having no fog:

| | multiplayer |
|---|---|
| radar off | black field, dots for the viewing house's own units **and** structures |
| radar on | terrain, plus enemy units — as today |

So three changes in that one function: own units draw in the radar-off branch
(today only structures do), `isUnveiled` drops out of the condition entirely
because v1 has no fog, and `g_playerHouse` / `g_playerHouseID` become the viewing
house.

None of this can desync — it is all presentation, and the minimap is drawn from
state it only reads. The blocking animation is the only part of the radar that
touches the simulation clock, and it is the only part that must go.

## The command layer

The pleasant surprise: the surface is narrow. Everything a player can do to the
world funnels through a handful of calls.

| Call | Site |
|---|---|
| `UnitSelection_IssueOrder` | [unit.c:2685](src/unit.c:2685) |
| `UnitSelection_IssueDefaultOrder` | [unit.c:5156](src/unit.c:5156) |
| `UnitSelection_ApplyPendingAction` | [unit.c:5385](src/unit.c:5385) |
| `UnitSelection_OrderHunt` | [unit.c:5288](src/unit.c:5288) |
| `UnitSelection_BeginAirTransit` | [unit.c:5322](src/unit.c:5322) |
| `Unit_SetAction` from the action panel | [widget_click.c:360](src/gui/widget_click.c:360) |
| `Structure_BuildObject` | [widget_click.c:131](src/gui/widget_click.c:131), [widget_click.c:461](src/gui/widget_click.c:461), [viewport.c:532](src/gui/viewport.c:532) |

plus placement confirmation, repair, the Starport order, the Palace weapon, and
harvester deploy/return.

So: `MpCommand { u8 type; u8 houseID; u16 a, b; }` — six to eight bytes — and one
entry point, `Mp_Execute(cmd)`. The GUI stops calling the engine directly and
*emits* a command; the turn stepper applies it through the same functions.

Selection, camera, control groups and the minimap are **local and never
networked**. One trap there: selection currently reaches into simulation state —
`Unit_Select()` sets a guard position ([unit.c:3783](src/unit.c:3783)). That has
to be separated, or "I clicked on a unit" becomes a desync.

## AI

Almost free, and this is the second reason lockstep beats a state-syncing server
here: **the AI already lives inside the simulation.** `doctrine.c` and the hooks in
`skirmish.c` / `unit.c` run identically on both clients as long as they read only
simulation state and the simulation RNG. Nothing is sent for an AI house at all.
The requirement is the same determinism discipline as everything else, so mixed
matches — two humans plus two AI houses — fall out of the match descriptor without
new machinery.

## Stages

Each stage is verifiable on its own, which matters because there is no test suite.

| # | Work | Verified by |
|---|---|---|
| 0 | **done** — `--mp-checksum`: CRC of the serialised state every K ticks, per savegame chunk | three runs of the same binary produce the same log; a second platform and compiler still to do |
| 1 | **done** — split the RNG into simulation and UI streams; seed the simulation per match | five runs identical, `--war-metrics` unchanged, three self-tests pass |
| 2 | **done (v1)** — match descriptor replacing `g_playerHouseID` in the logic; skirmish becomes a case of it | state byte-identical, the sixteen `--war-metrics` numbers unmoved |
| 3 | **done** — command layer plus local replay: record commands, replay, compare checksums | `--mp-replay` passes on three seeds; dropping one command fails it |
| 4 | network-gated stepper, render decoupled, `Mp_Pump()` in every nested loop (§5 v1) | two processes on localhost; modal-surface test from §5 |
| 5 | relay and lobby: rooms, join codes, timeout, config hash handshake | a match over the internet |
| 6 | per-house fog, non-modal build panel (§5 v2), reconnect by state upload, more than two houses, AI slots, spectators | |

**Stage 3 deserves the emphasis.** A local replay with no network at all proves the
choke point is complete: if any player action bypasses `Mp_Execute`, the replay
diverges and you see it before touching a socket. The side effect is a replay
feature.

## Stage 0 — the determinism harness (done)

```bash
cd bin
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --skirmish=ordos,harkonnen --mp-checksum=20000,5000
```

`--mp-checksum=ticks[,step[,seed]]` seeds both generators from the match seed,
starts a war skirmish on that seed, takes the clock away from the wall
([src/mpsync.c](src/mpsync.c), the runner in
[src/opendune.c:1168](src/opendune.c:1168)) and prints one line per sample:

```
mp-checksum t5000    total 4932d9f9  unit 3bc50a10 str e3b46d2d house 72ff4b50
                     map 4c6d67cb team 909a4570 new 488b2c13 info c0853027 rng 438f3e04
```

One CRC per savegame chunk rather than one number for everything, so a mismatch
says *what* diverged. The checksum goes through the savegame writers
(`Unit_Save`, `Map_Save`, …) into a `tmpfile()` rather than hashing the structs:
`Object` embeds a `ScriptEngine` holding two pointers
([script.h:42](src/script/script.h:42)) which differ between processes, struct
padding is not guaranteed initialised, and the savegame already writes named
fields big-endian with the script pointer converted to an offset. The generators
get their own component because they are in no chunk at all.

Verified: eight consecutive 20000-tick runs identical, and three 150000-tick runs
identical; a different seed changes every component; sampling every 1000 ticks
gives the same values at t5000 and t10000 as sampling every 5000, so taking a
checksum does not perturb the run. The regression suite is unmoved —
`--war-metrics` PASS with 0 regressions, and the three self-tests pass.

### What it found

**1. `Skirmish_Start*()` seeds nothing.** `Map_CreateLandscape(seed)` is seeded,
but everything after it — the base jitter, the corner choice, the spice fields
([skirmish.c:2157](src/skirmish.c:2157) onwards) — draws from the LCG, whose state
at that moment is whatever `OpenDune_Init()` left there:

```c
Tools_RandomLCG_Seed((unsigned)time(NULL));   /* src/opendune.c:1663 */
```

Two runs on the same map seed therefore got different bases and different spice.
`warsearch.c` and `ecosearch.c` work around this by seeding at the call site,
which is why the searches are reproducible and a plain `--skirmish` is not. The
harness does the same for now; **the seeding belongs inside `Skirmish_Start*()`**
and moves there in stage 1.

**2. `Tools_Random_256()` is seeded by nobody.** `Tools_Random_Seed()` has no
callers anywhere, so that stream always starts from an all-zero state. Reproducible
today by accident; in multiplayer it must come from the match seed like everything
else.

**3. `g_timerGame` is contaminated by the wall clock.** With the seeding fixed,
every component matched byte for byte except `info`, which stores *elapsed*
scenario time ([info.c:101](src/saveload/info.c:101)) — and the 60 Hz ticker adds
ticks of its own on top of the ones the loop asks for.

This one was worth chasing to the bottom, because the first fix looked like it
worked and did not. Switching `TIMER_GAME` off after the match was set up gave
three identical runs in a row and then diverged on the fourth: `Skirmish_Start*()`
ends with `g_tickScenarioStart = g_timerGame`
([skirmish.c:2218](src/skirmish.c:2218)), and a wall-clock tick landing in the
short window between that line and the loop is one stray tick of elapsed time. It
lands maybe one run in four. **A determinism bug that reproduces three times out
of four looks exactly like a determinism proof** — the harness has to be run in a
loop, not twice.

The fix is to take the clock before the match exists, not after:
`Timer_SetTimer(TIMER_GAME, false)` and `Timer_ResetGame()`
([timer.c:377](src/timer.c:377)) ahead of `Skirmish_StartWar()`, then
`Timer_StepGame()` to advance it. The simulation clock becomes a function of steps
taken rather than of how long the machine took to take them, and every absolute
tick value in the state is reproducible with it. That is §4's network stepper in
embryo, and it is the first piece of it that exists.

Worth noting the same contamination reaches the existing tools:
`--economy-baseline` repeats its build orders but not its spice totals between
runs. "Deterministic" in [CLAUDE.md](CLAUDE.md) is true of the plan it finds, not
of the number beside it.

### What it does not prove yet

One machine, one compiler, one libc. Cross-platform determinism — the same log
from a different build — is the next thing to run, and it is what decides whether
`-fwrapv` and a UBSan pass are needed before stage 1.

## Stage 1 — the RNG split (done)

Two generators now, and the rule is one sentence: **nothing that can reach saved
state may draw on the presentation stream, and nothing that only reaches the
screen or the speakers may draw on the simulation stream.**

`Tools_RandomUI_Range()` ([tools.c:298](src/tools.c:298)) is the new one. It is
the same LCG with its own state, seeded once from `time(NULL)` and never again —
nothing it feeds is ever compared between machines. Twenty-seven call sites moved
onto it: the four `Music_Play()` draws in the game loop, the one in `GUI_Mentat_Show()`,
the mentat's mouth and eyes, the four screen dissolves, the security question,
and the coin-flip between two sound effects in the viewport.

The simulation stream keeps everything else, including three that look like
presentation and are not:

| Kept on the simulation stream | Why |
|---|---|
| `explosion.c` | writes the crater into `g_map[].overlayTileID` ([explosion.c:105](src/explosion.c:105)) |
| `animation.c` | `Animation_Func_SetOverlayTile` / `SetGroundTile` write `g_map` ([animation.c:105](src/animation.c:105), [animation.c:133](src/animation.c:133)) |
| `GUI_FactoryWindow_CalculateStarportPrice()` | decides what the player pays ([gui.c:2737](src/gui/gui.c:2737)) — and both clients must agree on the price |

The Starport price is worth a second look later: the formula is duplicated in
`skirmish.c:1692` for the AI, and computing a price in the GUI is the wrong side
of the command layer.

### Per-match seeding

`Skirmish_StartInternal()` now seeds the LCG from the match seed
([skirmish.c:2124](src/skirmish.c:2124)). Nothing did before: `Map_CreateLandscape()`
took the seed, but the base jitter, the corner choice and the spice fields drew on
whatever `OpenDune_Init()` had left in the LCG from `time(NULL)`, so two runs of
the same map seed got different bases. The searches worked around it at the call
site, which is why they repeated and a plain `--skirmish` did not; those
workarounds are now redundant but harmless.

**A correction to stage 0.** That section claimed `Tools_Random_Seed()` had no
callers and the 256 stream always began from all zeros. It is wrong:
`Map_CreateLandscape()` calls it with the map seed
([map.c:1471](src/map.c:1471)). The 256 stream was already seeded per match, which
is why adding a second seeding call to `Skirmish_StartInternal()` changed nothing
measurable and why the sixteen metrics did not move. The call was removed again.
The original claim came from grepping four files instead of the tree.

### What the split found: the render path is part of the simulation

`Explosion_Tick()` and `Animation_Tick()` are not in the game loop at all. They
are called from `GUI_DrawScreen()` ([gui.c:4593](src/gui/gui.c:4593)), at frame
rate, scheduled against `g_timerGUI` — and they write craters and ground tiles
into `g_map`, which is saved state.

So a piece of the simulation currently runs on the render clock. Two clients do
not draw at the same rate, which makes this a desync by construction, and it has
nothing to do with the RNG split — the split merely forced the question of which
side of the fence those files were on.

It was also a hole in the harness, which never draws. `--mp-checksum` now owns
the GUI clock as well (`Timer_StepGUI()`, `Timer_ResetGUI()`) and steps both tick
functions from the stepper, on the game clock. The checksums moved when it was
added, which is the proof that those ticks do mutate saved state, and they repeat
across runs, which is the proof that they are deterministic once their schedule
is. **In a match the two clocks have to become one:** explosions and animations
belong in the stepper of §4, not in `GUI_DrawScreen()`.

### Cross-compiler, as far as this machine allows

There is no second compiler here — `/usr/bin/gcc` is Apple clang. The next best
thing is to vary what a second compiler would vary:

| Build | Result |
|---|---|
| `-O2 -fomit-frame-pointer` (release) | baseline |
| `-O0 -fwrapv` | byte-identical checksums |
| `-O1 -fsanitize=undefined,integer` | byte-identical checksums, one report in 150000 ticks |

The one report was real: `o = &Unit_Create(...)->o` in `Structure_BuildObject()`
takes a member address off a NULL pointer when the unit pool band is full, and
the check three lines down is written expecting the NULL back
([structure.c:1844](src/structure.c:1844)). Every compiler folds it to NULL
because `Object` sits at offset 0, which is exactly the kind of thing that holds
until it does not. Both creates are guarded now, and the checksum is unchanged.

Zero UBSan reports over 150000 ticks after that, across integer overflow, shifts,
alignment and division. A real second platform is still worth doing, but the
cheap proxies all agree.

## Stage 2 — the match descriptor (done, v1 scope)

[src/match.c](src/match.c) holds it: two slots, a house and a controller each,
and five questions the rest of the engine can ask.

```c
Match_IsActive()                  /* is a match set up at all */
Match_IsNetworked()               /* both slots human -> lockstep */
Match_IsHumanControlled(houseID)  /* what the simulation actually wanted */
Match_AreEnemies(h1, h2)
Match_GetOpponent(houseID)
```

The conversion rule is one line: wherever the simulation asked
`houseID == g_playerHouseID` to mean *is this the human*, it now asks
`Match_IsHumanControlled(houseID)`. `g_playerHouseID` keeps its other meaning —
the house the local screen belongs to — and every presentation site was left on
it deliberately.

**The rule is behaviour-preserving by construction**, which is what made it
checkable:

| Situation | Old test | New test | Same? |
|---|---|---|---|
| campaign | `h == g_playerHouseID` | descriptor inactive, so `h == g_playerHouseID` | yes |
| skirmish, AI house | false — the spectator owns nothing | slot controller is AI | yes |

So the state had to come out byte-identical, and it did: three 150000-tick runs
identical to each other and to the stage 1 baseline, every chunk. `--war-metrics`
PASS with all sixteen numbers unmoved, three self-tests pass.

`House_AreAllied()` lost its `Skirmish_IsActive()` patch and asks the descriptor
instead ([house.c:377](src/house.c:377)). With two slots the answer is the whole
matrix — in a match, anyone who is not you is against you.

### What moved

Roughly thirty sites, all of them places where the engine chose between
human-style and AI-style behaviour:

| Where | The fork |
|---|---|
| [unit.c:3153](src/unit.c:3153), [unit.c:3797](src/unit.c:3797), [unit.c:4139](src/unit.c:4139) | default action versus `ui->actionAI` |
| [unit.c:4195](src/unit.c:4195) | AI units take a toughness penalty |
| [unit.c:4538](src/unit.c:4538) | `ACTION_AMBUSH` becomes attack for AI only |
| [unit.c:3783](src/unit.c:3783) and the autonomy hooks | guard posts and manual attack positions, this fork's own features |
| [structure.c:328](src/structure.c:328), [structure.c:568](src/structure.c:568), [structure.c:457](src/structure.c:457) | AI build speed cap, instant upgrades, self-repair |
| [structure.c:1204](src/structure.c:1204) | refund on cancel differs |
| [structure.c:613](src/structure.c:613), [689](src/structure.c:689), [716](src/structure.c:716) | build-location strictness applies to humans only |
| [script/structure.c:133](src/script/structure.c:133) | the AI's harvester credits are jittered |
| [script/structure.c:651](src/script/structure.c:651), [script/team.c:497](src/script/team.c:497) | AI-only script paths |

### What deliberately did not move

Presentation keeps `g_playerHouseID`: sound feedback, on-screen text, the action
panel, control groups, cursor hostility, `g_scenario` tallies. So do the fog sites
(§2) and the radar (§6) — those are their own stages, and converting them here
would have moved state for reasons that have nothing to do with who is playing.

### One line that would have desynced without changing anything

`u->o.script.variables[3] = g_playerHouseID` ([unit.c:2968](src/unit.c:2968))
hands the EMC script a house id every tick. Disassembling `UNIT.EMC` with
[tools/dis_emc.py](tools/dis_emc.py) says the scripts never read it: across every
unit type they touch variables 0, 1 and 4 and nothing else.

But `variables[5]` is a **saved** array
([saveload/scriptengine.c:29](src/saveload/scriptengine.c:29)), so two clients
would have written their own viewer's house into a field the checksum covers and
no behaviour depends on — a desync visible only to the detector, which is worse
than a real one, because there is nothing to find at the other end. In a match it
now gets the opponent, which both clients compute alike.

The prediction was that the `unit` chunk would move and nothing else would. It
came out exactly so — `str`, `house`, `map`, `team`, `new`, `info` and `rng` all
byte-identical, and the sixteen metrics unmoved.

### Left for v2

The alliance matrix as data, more than two slots, and networked matches with AI
houses in them. None of it is needed for two people on one map.

## Stage 3 — the command layer and the replay (done, with a caveat)

[src/mpcommand.c](src/mpcommand.c) is the choke point. Everything a player does to
the world becomes an `MpCommand` and goes through `MpCommand_Submit()`; nothing
reaches the simulation any other way.

```c
typedef struct MpCommand {
	uint8  type, houseID, action, count;
	uint16 packed;                 /* target tile */
	uint16 object;                 /* structure index */
	uint16 value;                  /* what a factory should build */
	uint16 unit[MP_COMMAND_UNITS_MAX];
} MpCommand;
```

**A command names its recipients.** It cannot say "the selection", because the
selection is local and the other client has one of its own. So the group logic
this fork added — sorting recipients by distance, spreading a Move across tiles,
deciding whether an Attack is really an advance — moved out of the selection
functions into appliers that take a list:

| Was | Is |
|---|---|
| `UnitSelection_ApplyPendingAction()` | collects the list, submits; `UnitSelection_ApplyOrderToList()` applies it |
| `UnitSelection_IssueDefaultOrder()` | same split, `…ApplyDefaultOrderToList()` |
| `UnitSelection_BeginAction()` immediate branch | `…ApplyActionToList()` |
| `UnitSelection_OrderHunt()` | `…ApplyHuntToList()` |
| air transit | `…ApplyAirTransitToList()` |

Hostility moved with them: `UnitSelection_IsHostileTarget()` takes the *issuing*
house now rather than reading the viewer, so both clients resolve a right-click
the same way.

Locally `Submit()` records and executes in the same breath. In a networked match
that is where a command is stamped with turn T+D and handed to the relay instead,
and `Execute()` runs when every player's packet for that turn has arrived.

### The replay

```bash
./opendune --skirmish=ordos,harkonnen --mp-replay=40000,500
```

Two passes in one process. The first plays the match with a scripted player
issuing orders through `MpCommand_Submit()`, recording each with its tick. The
second replays that recording into the same match with the scripted player
switched off, and compares the per-chunk checksum at every sample. Samples are
taken **on the command's own tick, after it runs** — sampled a step later the AI
has re-ordered the same units in the meantime, and a dropped command leaves no
trace.

Passes on three seeds. Dropping one command from the replay fails; dropping all
of them fails.

### What it found, both times by failing

**1. Two matches in one process were not the same match.** The second pass
diverged with no commands involved at all. Every subsystem keeps its next-run
deadline as an absolute tick — `s_tickUnitScript`, `s_tickStructureScript`,
`s_tickHouseHouse`, `s_tickTeamGameLoop` and a dozen more — and those are module
statics that survive a match. Start the clock at zero again and the deadlines
from the last match are all in the future, so nothing runs until the clock
catches up. `Unit_ResetTicks()`, `Structure_ResetTicks()`, `Team_ResetTicks()` and
`House_ResetTicks()` are called from `Game_Init()` now.

**This moved the sixteen metrics**, and the move is worth being explicit about
because it is not a regression and not noise:

| | before | after |
|---|---|---|
| turret.entries/match | 16 | 20 |
| turret.dwell/match | 8684 | 7659 |
| harv.lost/match | 0 | 2 |
| econ.spice/match | 66826 | 62418 |
| result.points % | 83 | 70 |
| result.wipeouts % | 8 | 25 |
| verdict | PASS, 7 of 16 short of goal | PASS, 12 of 16 short of goal |

Every gate still holds, and the numbers repeat exactly between runs. What changed
is that a match now starts from the same state whether it is the first in the
process or the fifth — before, the first match began with every scheduler at zero
and every later one began with the previous match's deadlines a few ticks in the
future. The searches were measuring a mixture of the two. `result.wipeouts` is now
sitting exactly on its gate at 25, which is worth watching. Re-baselining
[metrics.md](metrics.md) against the corrected behaviour is a decision for
whoever owns those numbers, not a side effect of this stage.

**2. The first version of the test was vacuous.** It passed with the *entire*
recording deleted. Unit orders go through `UnitSelection_IsControllable()`, which
refuses anything no human controls, so every order the scripted player issued to
an AI house was silently a no-op. Only the negative control caught it — the
positive result looked perfect throughout.

The obvious repair does not work either: flagging one of the skirmish AI's houses
as human-controlled stops it building, because an AI house that runs out of money
gets its production put on hold and clearing the hold is a player action nobody
performs. So the scripted player orders *production* instead, which does reach an
AI house.

That is the caveat on this stage. The replay proves recording and executing a
command stream is faithful, and it proves the plumbing end to end. It does **not**
yet exercise the unit-order commands, and it cannot until there is a human-versus-
AI mode with a house the AI does not also drive.

### Not yet routed

`Structure_Place()` — the placement click also creates the free harvester,
records the palace position and changes selection type, all in the viewport
handler ([viewport.c:509](src/gui/viewport.c:509)); untangling it belongs with the
non-modal build panel of §5. Repair, the Starport order, the Palace weapon, the
rally point and the production queue are still direct calls. Each is a command
waiting to be written, and the replay is how each will be checked.

## Stage 3a — the same recording in a second process (done)

Stage 0 proved that a match repeats. Stage 3 proved that a recording of what the
player did is enough to reproduce the match. Both ran inside **one process**, and
one process is the weakest possible place to prove either: static-initialisation
order, address-space layout, whatever the environment leaked in — all of it is
held constant for free, and none of it will be constant between two players.

So the recording now goes to a file, and the second pass is a second process.

```bash
./opendune --skirmish=ordos,harkonnen --mp-replay=40000,500 --mp-record=rec.mpc > a.log
./opendune --skirmish=ordos,harkonnen --mp-replay=40000,500 --mp-play=rec.mpc   > b.log
diff <(grep '^mp-checksum' a.log) <(grep '^mp-checksum' b.log)
```

`--mp-record=FILE` and `--mp-play=FILE` are modifiers on `--mp-replay`, which
keeps its parameters (`ticks,step,seed`) and its meaning; with neither modifier
it still runs both passes in one process and decides for itself. With a modifier
it runs **one** pass and prints its samples, because a single process has no
standing to judge — `diff` does. Both halves share one function
(`MpHarness_ReplayPass()`), so the recorded and the replayed match cannot drift
apart through two copies of the loop.

The recording is text, one command per line, with the seed in the header:

```
opendune-commands 1
seed 1000
count 80
cmd 500 6 2 255 65535 0 1 0
```

Text rather than a packed struct on purpose. The whole value of a recording is
that it can be read when a replay disagrees, and a line per command diffs where a
binary blob only says "different" — as the negative controls below show, it names
the tick. The seed line is a refusal, not a comment: a recording replayed onto
another map would diverge for a reason that has nothing to do with the command
layer, which is exactly the false alarm a desync hunt does not need.

### The result

81 samples over 40000 ticks, byte-identical between the two processes, all eight
components. Two producer runs also wrote the same recording file.

### The negative controls, which matter more

A cross-process test that passes proves nothing until it can also fail:

| Tampering | Result |
|---|---|
| Drop one command (line 45 of 80, tick 21000) | diverges, first at **t21000** |
| Shift one command by a single tick (21000 → 21001) | diverges, first at **t21000** |
| Change the seed in the header | refused before the match starts |

The one-tick shift is the interesting one. That command still executes, and the
consumer still reports 80 of 80 played — it is *only* the checksum log that
catches it. Which is the point: the tally is bookkeeping, the log is the test.

### What this still does not cover

The recording is 80 commands and every one of them is `MP_CMD_STRUCTURE_BUILD`,
for the reason stage 3 recorded — the unit-order commands cannot reach a house
the AI drives. Two processes agreeing about production is worth having, but the
unit-order half of the command layer is still untested against anything, and no
amount of process separation fixes that. It needs a house a human controls and
the AI does not, which is the next piece of work.

Nothing here touches the simulation: `--mp-checksum` is unchanged between runs
and the in-process `--mp-replay` still passes.

## Stage 3b — a house the AI does not drive (done)

Stage 3 recorded its own gap plainly: every command in the recording was a build
order, because `UnitSelection_IsControllable()` refuses a unit no person
controls, and both skirmish houses were the AI's. The unit half of the command
layer — the half a player spends the whole match in — had never been executed by
anything.

`--human=N` marks skirmish slot N as played by a person (`--human=1,2` for both).
It is a controller on the match descriptor, not a new mode: everything that
decides whether a house is the AI's already asks `Match_IsHumanControlled()`.

### What had to stop happening to that house

* **Engine teams and the doctrine.** `Skirmish_StartInternal()` no longer creates
  either for a human house. Two things steering one army is how a player's orders
  get quietly overwritten a tick later.
* **The base plan.** It stays, but as advice: nothing works through it any more.
* **Free placement.** This one was hiding in plain sight
  ([structure.c:378](src/structure.c:378)). When a Construction Yard finished,
  one branch on `g_playerHouseID` answered two different questions — *who gets
  told* and *who puts the building down*. A second person's house is neither the
  campaign player nor the AI, so it fell into the AI's arm and got its buildings
  placed for free. The recording said so before the code did: eleven build
  commands, zero placements, and a base standing on the map anyway.

### What had to start being a command

| Command | Why it could not stay in the GUI |
|---|---|
| `MP_CMD_STRUCTURE_PLACE` | `Structure_Place()`, plus the Palace position and the Refinery's free harvester — which was created for `g_playerHouseID`, i.e. for the wrong player the moment two can build |
| `MP_CMD_STRUCTURE_HOLD` | hold and resume; a house nobody resumes stops building the first time it runs out of money |

The placement command names the **yard**, not the building. Clearing
`linkedID` is simulation state, and the GUI used to do it when the player pressed
"Place it" and put it back if they cancelled — local bookkeeping that would have
desynced on the first placement, because only one client's player presses
anything. Cancel is now purely local, and a yard destroyed mid-placement takes
the unplaced building with it, which `Structure_Destroy()` already did.

Whether the spot is legal is still decided locally, from `g_selectionState`, and
that is not a shortcut: in lockstep the click cannot wait for the placement, so
the feedback has to come from a local test while the command runs two turns
later on both machines.

### The scripted player, second attempt

It now plays a house of its own, and three separate mistakes had to be walked out
of it — each one caught by the replay rather than by reading the code:

1. **It used the AI's answer to "where does this go".**
   `Skirmish_Plan_TakePosition()` marks the plan entry, appends to the build
   history and **lays the slabs** — map tiles and credits, changed outside the
   command layer. The recording was faithful and the two passes still disagreed
   at t500. It now finds a spot the way a person does, by scanning the base
   rectangle for a legal one, which reads state and changes none.
2. **It placed buildings that were not finished.** Placing while the yard is
   still counting down leaves the yard counting towards an object it no longer
   has, and it never builds again: forty thousand ticks, two commands, PASS. The
   command now refuses an unfinished building too, not just the caller.
3. **It built whatever the round-robin landed on** — a House of Ix, a Heavy
   Vehicle factory, a Barracks — was broke by t10000 and stood still for the rest
   of the match. It now opens with a Refinery and keeps the lights on.

### The result

296 commands over 40000 ticks: 11 builds, 5 placements, 75 resumes and **205
unit orders** — the half that had never run. 81 samples, identical in-process and
across two processes.

| Tampering | Result |
|---|---|
| Drop one unit order (t20000) | diverges from **t20000**, 41 of 81 samples |
| Drop one placement (t1600) | diverges from **t2000**, 77 of 81 samples |

AI against AI is untouched: the sixteen doctrine metrics are identical to the
previous run, which is what the spectator house explains — a skirmish sets
`g_playerHouseID = HOUSE_MERCENARY`, so no AI house was ever taking the branch
that changed.

### What it does not cover

The viewpoint. Both processes above run with the same `g_playerHouseID`, and in a
real match they do not: each client sees its own house. Every simulation site
still keyed on the viewpoint rather than on the match descriptor would diverge
there and cannot diverge here. Per-house fog (§2) is the known one; whether it is
the only one is the next thing to measure.

## Known hazards

* **The unit pool.** Two humans building freely will hit the per-type
  `indexStart`/`indexEnd` bands far harder than the campaign ever did — every
  ground unit of every house shares one band ([units.md](units.md)).
* **Sound and feedback inside game logic.** `Sound_Output_Feedback()` is called
  from simulation functions ([unit.c:4508](src/unit.c:4508),
  [structure.c:447](src/structure.c:447)). Acceptable while it only *reads* the
  view house and touches neither state nor the simulation RNG.
* **`Skirmish_IsActive()` escape hatches** must migrate into the match descriptor,
  or multiplayer inherits skirmish-only behaviour by accident.
* **Save format.** The match descriptor, the simulation seed and per-house fog are
  all saveload fields ([src/saveload/](src/saveload/)). A multiplayer save should
  ideally load on both sides and resume at a known turn.
* **The credit clamp and the truncated `buildable` mask** noted in
  [skirmish.md](skirmish.md) are engine quirks two humans will find quickly.
* **Undefined behaviour is a desync**, not just a warning: signed overflow,
  uninitialised reads, anything that depends on struct padding. Worth a `-fwrapv`
  and a UBSan pass over the simulation before blaming the network.

## Rejected

* **Authoritative server with state snapshots** — a delta protocol over EMC script
  state. See the top of this file.
* **Rollback / GGPO** — the latency it buys does not matter at 130 ms turns.
* **Peer-to-peer without a relay** — NAT.
* **Letting one client's state win a checksum mismatch** — it hides the bug and
  can corrupt the other player's game.
