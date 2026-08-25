# Multiplayer — deterministic lockstep over the internet

**Status: stages 0 to 6 are in the tree, the lobby included; what is left of the
interface work is listed in stage 5b.** The determinism
harness, the RNG split, the match descriptor, the command layer, the turn loop,
the relay, the real game loop and the lobby exist as code,
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

### What v1 actually cost (done)

The pump was the easy half. The world kept turning behind the window from the
day `Timer_PumpMatch()` went in ([timer.c:180](src/timer.c:180)) — and a player
who opened the mentat or the options screen still desynced the match, because
four separate places let the local interface write into the shared world.

`--mp-modal=ticks[,step[,seed]]` is the regression test this section asked for.
It plays the match twice and, in one of the passes, performs what a modal screen
does to the world — `GUI_ModalScreen_Enter()`, ticks of somebody reading,
`GUI_ModalScreen_Leave()`, the selection-type flip the build list uses, and the
recount the options screen carries. The event loops are deliberately not in it:
they are the part that was already solved. `--sim-purity` on the same run names
the call and the chunk; `--mp-modal-dump=TICK` leaves both passes' state on disk
for `tools/mpdesync_diff.py`. All four were found in six runs.

| What | Where | Why it moved the world |
|---|---|---|
| `Unit_Recount()` / `Structure_Recount()` on leaving Options | [widget_click.c](src/gui/widget_click.c) | Rebuilds `g_unitFindArray` in **index** order. The live order is creation order, worked towards front-to-back by one bubble pass of `Unit_SortOrder()` per tick — a function of the whole history, not of the set. `GameLoop_Unit()` walks that array, so one client began ticking its units in a different sequence. No chunk records this order, so no checksum saw it directly; it showed up later as everything |
| The `SELECTIONTYPE_MENTAT` gate in `Unit_HouseUnitCount_Add()` | [unit.c:7056](src/unit.c:7056) | The gate *is* the viewpoint. While one client had a fullscreen screen up it skipped `timerUnitAttack`, `timerSandwormAttack` and `t->script.variables[4] = 1` — the flag that tells a team it is under attack. Two clients, two different scripts, from the next tick |
| `upgradeTimeLeft = 100` in the action panel | [widget_draw.c:652](src/gui/widget_draw.c:652) | A draw function arming a saved field from whatever the local player had selected. Already guarded — on `MpTurn_IsActive()` |
| `Structure_UpdateMap()` on a full repaint, `Explosion_Tick()`/`Animation_Tick()` in `GUI_DrawScreen()` | [gui.c](src/gui/gui.c), [map.c](src/map.c) | Same: guarded on `MpTurn_IsActive()` |

The last two are the same mistake and it is worth naming, because it will happen
again: **`MpTurn_IsActive()` is not "are we in a match"**. It is "is the turn
loop running", and the match is built several hundred milliseconds earlier — a
window in which one screen fade is enough. Guards on shared state now ask
`Match_IsActive()`, or `Timer_AnimClockIsClaimed()` where the question is who
owns the simulation clock. `MpTurn_IsActive()` stays where it belongs: the
packet cadence, the opcode budget, and the buttons that must go dead while a
networked match runs.

Also closed here, from this section's own list: **Save, Load, Restart and Pick a
new house are refused while `MpTurn_IsActive()`** — the red row in
[mp-actions.html](mp-actions.html). Loading is the sharp one, but restart and
pick-a-house leave the opponent playing against nobody just as surely.

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
| 4 | **done** — the lockstep turn loop, with a loopback and a file transport | two processes playing one match; a measured ping table |
| 5 | **done** — a relay and a TCP transport: rooms, join codes, drop detection | two processes playing one match through a socket, on five seeds |
| 5b | **done** — the lobby: a room built from the menu, the config digest folded into its name | `--lobby-self-test`; a match started without a command line |
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
non-modal build panel of §5. The Starport order, the Palace weapon, the rally
point and the production queue are still direct calls. Each is a command waiting
to be written, and the replay is how each will be checked.

Placement and repair have since been routed (§6). The Starport is the one that
needs more than a command: its prices come from a generator seeded with the
*viewer's* house, so before a purchase can cross the wire both clients have to
agree what the buying house is being charged.

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

## Stage 3c — the viewpoint (done)

Everything measured so far ran with the same `g_playerHouseID` on both sides. Two
real clients never do: each sees the match from its own house. So the last cheap
thing to build before a turn loop was a harness that varies **only** that.

```bash
./opendune --skirmish=ordos,harkonnen --human=1,2 --mp-viewpoint=60000,1000
```

It is the replay harness with one change: pass 1 records from slot 1's chair,
pass 2 replays from slot 2's. Same commands, same match, same seed. Anything that
differs is the simulation reading who is watching, and the report names the
chunk and the tick. `--viewpoint=N` sets the chair by hand for a single run.

It opened at **80 of 81 samples differing** and is now at zero, on five maps.

### What was reading the chair

| Site | What it decided |
|---|---|
| [structure.c](src/structure.c) `Structure_GetBuildable` | **what a house may build** — prerequisites and upgrade levels enforced for the viewpoint's house, waived for everyone else. Moving the chair took the scripted player from 296 commands to 1 |
| [house.c](src/house.c) the credit clamp | which house got the no-silo grace; `g_playerCreditsNoSilo` is one global for one player. In a match it is now per house, kept with the base |
| [house.c](src/house.c) `House_UpdateRadarState` | `flags.radarActivated`, saved, updated only for the viewpoint. Now decided for any house; only the two seconds of STATIC.WSA stay with the screen — a step into §6 |
| [unit.c](src/unit.c) `Unit_HouseUnitCount_Add` | the sighting counters, the attack-warning timers and `t->script.variables[4]` all went to the watching house instead of the seeing one |
| [unit.c](src/unit.c) same, last line | a unit belonging to *the viewpoint* was marked seen by everybody; each client marked a different half of the map |
| [unit.c](src/unit.c) `Unit_RemoveFog`, `Unit_UpdateMap`, [map.c](src/map.c) `Map_UnveilTile`, five sites in `Structure_Place` | only the viewpoint's own units and buildings lifted fog, and lifting it counts whatever is standing there |
| [unit.c](src/unit.c) `Unit_RemovePlayer` | a dying unit left its team on one client and stayed in it on the other |
| [unit.c](src/unit.c) the autonomy scorer | scored off `s_houseThreatUntil[g_playerHouseID]` — the *other* player's alarms |
| [unit.c](src/unit.c) `Unit_CreateWrapper` | `byScenario`, a saved unit flag |
| [map.c:444](src/map.c:444) the splash-damage loop | **the one that took longest.** `if (u->o.houseID == g_playerHouseID)` reports a threat to the autonomy layer *and continues* — so on one client a hit unit answered with autonomous defence and on the other it acquired a target from the script. Two clients, two different fights |

Everything else that reads `g_playerHouseID` in those files is a message, a
sound, a hint or the cursor, and stays.

Two things that legitimately differ per client were taken out of the checksum
rather than made to agree: the six mission tallies and the score
(`killedAllied`/`killedEnemy` and their pair, `g_scenario.score`). Allied versus
enemy is a question only a viewpoint can answer, so two clients keep two correct
and different tallies. They feed nothing, and they ride in the savegame's info
chunk, so `MpSync_InfoSave()` stashes and restores them around the write.

### What the wider net caught on the way

Running the *replay* on five seeds instead of one turned up two failures that had
nothing to do with the viewpoint, and both were the same bug as stage 3's: a
second match in one process inheriting the first one's module state.

* `Explosion_Init()` and `Animation_Init()` cleared their arrays but not their
  rate limiters, which hold absolute deadlines. A second match skipped every
  explosion until the clock caught up — and whether that mattered depended on how
  long the first match had run, which is why it looked like a map-specific ghost.
* `Unit_ResetTicks()` cleared the schedulers and four arrays; it missed nine more
  — attack posts, autonomous posts, manual-hunt flags, the harvester trackers,
  the refinery claims, the threat targets — plus the selection and control
  groups, and `Structure_ResetTicks()` missed the build queues and rally points.
  All of them are indexed by pool index, and the pool hands the same index to a
  different object next match.

The lesson from stage 0 held again, one level up: **one seed is not a sample.**
The replay passed on seed 1000 for as long as seed 1000 was the only seed.

### Where it stands

| Harness | Maps | Result |
|---|---|---|
| `--mp-checksum` | 1 | two runs identical |
| `--mp-replay`, AI vs AI | 5 | PASS |
| `--mp-replay`, one human house | 5 | PASS |
| `--mp-replay`, two human houses | 5 | PASS |
| `--mp-replay` across two processes | 1 | 81 samples identical |
| `--mp-viewpoint` | 5 | PASS |
| the sixteen doctrine metrics | 6 | unchanged, to the number |

## Stage 4 — the turn loop, and two processes playing one match (done)

The command layer said *what* a player did. The turn loop says *when* it
happens, and it is the last piece before a socket.

`MpTurn_*` ([src/mpturn.c](src/mpturn.c)) holds a turn number, an outbox, and a
transport it knows nothing else about. `MpCommand_Submit()` gained one branch:

```c
if (MpTurn_IsActive()) { MpTurn_Submit(cmd); return; }
```

That single line makes the whole interface lockstep-ready, because everything
already submits — the build panel, the placement click, every unit order. A
command no longer happens when it is given: it goes into the packet for turn
`N + D`, and `MpTurn_Advance()` runs it there, on every client, in slot order
rather than arrival order.

`MpTurn_Advance()` returns false when somebody's packet has not arrived, and the
caller must then hold the simulation clock still. That stall is the only way
latency is ever allowed to show.

### Two transports, one interface

* **Loopback** — both slots in one process. Not a stand-in for the network: it is
  what a local match uses, and it is what lets the loop be tested without one.
  5001 turns over 40000 ticks, no stalls.
* **File** — two processes, one directory, a packet per `(slot, turn)` written
  beside its real name and renamed into place so a half-written packet is never
  read as a whole one. Each process runs its own entire simulation and sees the
  other only through packets, which is the shape a socket has.

```bash
./opendune --skirmish=ordos,harkonnen --human=1,2 --mp-turnloop=8000,500 --mp-net=1,/tmp/net &
./opendune --skirmish=ordos,harkonnen --human=1,2 --mp-turnloop=8000,500 --mp-net=2,/tmp/net &
```

**2006 packets exchanged, 17 checksum samples, identical.** Two processes, two
scripted players, one match — the thing this was all for.

Every packet carries a CRC of a past turn, so the two are comparable without
anybody being the authority, and both sides notice a disagreement independently.

| Tampering | Result |
|---|---|
| The two players on different maps | both report a disagreement about turn 0 |
| One player never joins | the other stalls and gives up: "no packet for turn 0" |

### The ping table, measured

`--mp-realtime` paces the harness at 60 Hz and `--mp-lag=ms` holds each packet
back at the **sender**. That last word is the whole experiment: a first version
withheld packets from whoever read them, timed from the moment they first
looked, so a packet that had been sitting there for two turns still cost a full
lag when somebody finally asked — and raising the turn delay changed nothing,
which is exactly the shape of a wrong model.

Twenty seconds of match, both players on one machine:

| Ping | TL | D | Budget | Stalled | Share of the match |
|---|---|---|---|---|---|
| 24 ms | 8 | 2 | 266 ms | 13 ms | 0.07 % |
| 100 ms | 8 | 2 | 266 ms | 50 ms | 0.25 % |
| 200 ms | 8 | 2 | 266 ms | 520 ms | 2.6 % |
| 400 ms | 8 | 2 | 266 ms | 3989 ms | **20 %** |
| 400 ms | 8 | 4 | 533 ms | 287 ms | 1.4 % |
| 400 ms | 16 | 2 | 533 ms | 458 ms | 2.3 % |

(This table was measured before the default delay became 3; the `D` column is
what was passed, not what a run without `--mp-turn` would do today.)

The budget is `D * TL` ticks, and the table says what the design argued: while
the ping fits inside it the match does not stutter at all, and when it does not,
it stutters badly. Both ways out work — a longer delay or longer turns — and both
are paid for in how long the player waits to see their own order take effect.
`--mp-turn=TL,D` is where that trade is made; adapting it to the measured ping
is v2.

Every configuration in the table agreed on every checksum.

### What is still missing before this is multiplayer

* **The real game loop.** The turn loop runs in the harness, which owns its own
  clock. In the game, `g_timerGame` is driven by a 60 Hz timer that does not stop
  — §4 of this document, still design.
* **A lobby**, and the modal windows and radar animation of §5 and §6. *(All
  three done — stages 5b, 6.)*

## Stage 5 — the relay and the socket (done; the lobby is not)

The file transport proved the turn loop, not the network. It had no loss, no
reordering, no NAT and no second machine — three of those still do not appear on
a localhost socket, but the code path does, and the code path is what stage 5
replaces.

### Why a relay and not a direct connection

Peer to peer needs hole punching, and behind symmetric NAT — most home routers,
every mobile network — it still fails. Both clients dialling **out** to one public
address works everywhere there is internet at all, and it gives room codes for
free: the room is the join code, and neither player has to know the other's
address or open a port.

The cost is one extra hop of latency each way, and the ping table below says what
that costs. The answer is: raise `D` by one.

### The relay knows nothing about the game

[tools/relay/relay.go](tools/relay/relay.go) is about three hundred lines of Go
and it never parses a command. A packet arrives, it goes to everybody else in the
room, and that is the whole of it. It holds no state a client could disagree
with and decides nothing — two clients that disagree about the world find out
from each other's checksums, not from the relay.

That is deliberate and it is the same principle as §"No player is the source of
truth", one level down: **a bug in the relay cannot become a bug in the match.**
It can only stop packets, and a stopped packet is a stall, which is visible.

The wire protocol is line-oriented and readable on purpose, because when a match
desyncs the packets are the evidence and watching a room with `netcat` is worth
more than the bytes it costs:

```
client -> relay   JOIN <room> <slot>\n
relay  -> client  WELCOME <slot> <members>\n
relay  -> client  READY <members>\n           once the room is full
relay  -> client  LEFT <slot>\n               when somebody drops
client -> relay   PKT <length>\n<length bytes>
relay  -> client  PKT <slot> <length>\n<length bytes>
```

The slot in an outgoing `PKT` is filled in by the relay from the connection it
arrived on, never from what the sender claims: a client cannot speak for its
opponent.

**TCP, not UDP.** At the default turn length a client sends 7.5 packets per
second of a few dozen bytes each — a match is under a kilobyte per second in both
directions together. Head-of-line blocking, the usual reason to avoid TCP, costs
one turn here, and lockstep was going to wait for that packet anyway. What TCP
buys in return is ordering, retransmission and NAT traversal that already work,
on every platform, with no code. The transport sits behind `MpTransport`, so if
that judgement turns out wrong it is one file.

### The socket side

[src/mpnet.c](src/mpnet.c) is the third `MpTransport`. The join is done with the
socket still blocking, because there is nothing to do until it succeeds;
everything after is non-blocking, because the game loop may never wait on the
network anywhere except the deliberate stall in `MpTurn_Advance()`.

Two details are worth naming:

* **Our own packet never round-trips.** `send` stores the local packet straight
  into the receive window. Hearing our own move back from the relay before we
  could act on it would put a whole ping into every turn for nothing.
* **TCP is a stream, so a packet is not a read.** `MpNet_ParseBuffer()` consumes
  whole messages and leaves a partial one in the buffer for the next pump. This
  is the bug that would otherwise appear only under load, on a real link, in
  front of a player.

### The result

```bash
tools/relay/relay -listen :31337 -verbose
./opendune --skirmish=ordos,harkonnen --human=1,2 --mp-turnloop=20000,1000 --mp-relay=HOST:31337,room,1 &
./opendune --skirmish=ordos,harkonnen --human=1,2 --mp-turnloop=20000,1000 --mp-relay=HOST:31337,room,2 &
```

Twenty thousand ticks, 21 checksum samples, **identical on all five seeds tried**
— 1000, 7919, 24757, 31337, 40595. One seed is not a sample; that lesson is
recorded twice already in this file.

### The negative controls

| Tampering | Result |
|---|---|
| The two players on different maps | both report a disagreement about turn 0, independently |
| One player never joins | "the other player never joined", at the lobby, before a tick is simulated |
| One player quits mid-match | "slot 2 left the match at turn 253" |

The last one is the one that changed the code. The first version noticed nothing
and sat out the full twenty-second packet timeout, because our own socket was
still perfectly healthy — it is the *relay* that knows the other player is gone,
and it says so with `LEFT`. Twenty seconds of a frozen screen is what that bug
would have looked like to a player. Acting on `LEFT` turned it into an immediate
answer.

### The ping table again, over a real socket

`-lag` on the relay holds every forwarded frame back by a fixed delay, so two
processes on one machine can be made to feel like two players on different
continents. It lives in the relay rather than in the game on purpose: a client
that could tell a slow relay from a slow opponent would be a client with a second
source of truth.

Thirty seconds of match, paced at 60 Hz:

| Ping | D | Budget | Stalled, slot 1 | Stalled, slot 2 |
|---|---|---|---|---|
| 0 ms | 2 | 266 ms | 0 ms | 0 ms |
| 25 ms | 2 | 266 ms | 38 ms | 21 ms |
| 75 ms | 2 | 266 ms | 88 ms | 72 ms |
| 150 ms | 2 | 266 ms | **2434 ms** | **2457 ms** |
| 150 ms | 3 | 400 ms | 153 ms | 154 ms |
| 150 ms | 4 | 533 ms | 161 ms | 146 ms |

Every row agreed on every checksum, including the stuttering one: a stall is not
a desync, it is the loop doing its job.

The interesting row is 150 ms at `D=2`. The budget is 266 ms and the ping is
150 ms, so on paper it fits with room to spare — and it does not. What the file
transport's clean arithmetic hid is that the remaining 116 ms has to absorb the
scheduler, the relay's own hop and the jitter of both, and it does not always.
**The usable ping is well under the budget, not equal to it**, and the margin is
what `D` is for. At 150 ms, `D=3`.

### Deployed, and what the real internet said

The relay runs on a VDSina VPS in Moscow that was already carrying a LiteLLM
proxy — one core, 1.6 GB, nginx on 80 and 443, the proxy bound to localhost. The
relay took port 31337, about a megabyte of memory and no measurable CPU, under
`DynamicUser=yes` with the filesystem, devices and address families locked down,
because it must not be able to reach its neighbour's secrets.

Measuring the link taught something the localhost runs could not:

| | |
|---|---|
| `connect()` | 0.3 ms |
| First application round trip | 650–870 ms |
| Round trip on a warm connection | median **93 ms**, p90 170 ms |

The first two lines are a transparent proxy on this machine's path: it completes
the TCP handshake locally and only then dials out, so the connection *looks*
instant and the first byte costs most of a second. Only the third line is the
number the match runs on — **the join is slow and the match is not**, and a
naive one-shot ping measurement would have reported the wrong figure by a factor
of seven.

Three matches, three seeds, through the real relay: **identical**. That is the
first test with real loss, reordering and NAT in it.

Thirty seconds paced at 60 Hz, both clients on one machine so both legs share
the path:

| TL | D | Budget | Stalled, slot 1 | slot 2 | Share |
|---|---|---|---|---|---|
| 8 | 2 | 266 ms | 1303 ms | 1304 ms | **4.3 %** |
| 8 | 3 | 400 ms | 131 ms | 131 ms | 0.4 % |
| 8 | 4 | 533 ms | 228 ms | 119 ms | 0.6 % |
| 16 | 2 | 533 ms | 199 ms | 126 ms | 0.5 % |

The emulated table predicted this exactly: a 93 ms median with a 170 ms p90 does
not fit a 266 ms budget, because the budget has to cover the p90 and the
scheduler, not the median. `D=3` is clean and `D=4` buys nothing further.

**So `MP_TURN_DELAY_DEFAULT` is 3, not 2.** The default should be the one that
works on the transport v1 actually uses, and every real match goes through a
relay. It costs 133 ms more between the click and the order taking effect —
400 ms in a game where a tank takes a second and a half to turn around. Choosing
it from the measured ping instead of a constant is v2; `--mp-turn=TL,D` is the
manual override until then.

### What is still missing after this

* **A lobby.** Room codes exist on the wire and nowhere in the interface: today
  both players type a command line. This is the next piece of work. *(Done —
  stage 5b.)*
* **The real game loop**, still. `--mp-turnloop` owns its clock; the game does
  not (§4).
* **A handshake.** Nothing yet checks that the two clients are the same build
  with the same `opendune.ini` — the two most likely causes of a desync between
  two real people, and the two easiest to catch before the match instead of at
  turn 0. *(Done — stage 5b folds a digest of both into the room name, so a
  mismatch cannot start a match at all.)*

## Stage 6 — the real game loop, and what it cost (done; the interface is not)

`--mp-relay` on its own, without `--mp-turnloop`, is the actual game: two
windows, two players, the map drawn, the mouse live, and the simulation stepped
by the turn loop underneath it.

```bash
tools/mpduel.sh                       # both sides on this machine, through the relay
tools/mpduel.sh --seed=1234 --units=0 # a named map, no starting squad
```

The script is the short way to start both clients; the long way is two command
lines differing only in the slot:

```bash
./opendune --skirmish=ordos,harkonnen --human=1,2 --speed=2 --mp-units=4 \
           --mp-relay=146.103.110.160:31337,room1,1
```

`--speed=N` sets the tick multiplier, `--mp-units=N` puts a starting squad next
to each base (there is nothing to order about otherwise), `--mp-seed=N` names
the map, `--mp-sample=N` how often a checksum is logged.

### The stepper

`MpGame_Step()` is the whole loop. The wall clock says how many ticks are *due*,
the turn loop says how many may run, and drawing gets the time left over. A
stall returns immediately instead of spinning, so a missing packet freezes the
world and not the program.

It is registered as `Timer_SetMatchPump(&MpGame_Pump)` and called from the timer
idle hook as well as from the frame loop, which is what keeps the world running
while a player is inside the fullscreen build screen — a modal screen in this
engine is a loop that owns the process, and in a match it may not own the world.

### What had to stop being presentation

Every one of these was a place where drawing, input or the wall clock reached
into the simulation. They are listed in the order they were found, because the
order is the lesson: each fix uncovered the next.

| What | Why it desynced |
|---|---|
| `Unit_Sort()` in `GUI_DrawScreen()` | reordered the simulation's unit array once per drawn frame |
| the INFO chunk | carried the selection, the active structure, the hints — the chair, saved |
| `upgradeTimeLeft` armed in `widget_draw.c` | a draw function starting a countdown |
| `Timer_SetTimer(TIMER_GAME, true)` | half a dozen screens handed the game clock back to the wall |
| `radarActivated` | set by the spectator convenience in `House_UpdateRadarState()` |
| `g_hintsShown1/2` | a note about the person, stored in the world |
| `Structure_UpdateMap()` from `Map_SetSelection()` | a click restamping tiles |
| `isDirty` / `isHighlighted` | renderer instructions living in the savegame |
| the script opcode budget | 52 opcodes on screen, 3 off it — two screens, two speeds |
| `g_timerGUI` driving explosions and animations | craters and ground tiles on the render clock |
| `Explosion_Func_ScreenShake()` | sleeps inside a simulation step, and sleeping re-entered the stepper |
| `GUI_FactoryWindow_InitItems()` | reseeded the *game* LCG, from the viewer's own house, on opening a window |
| `GUI_Widget_TextButton_Click()` with one unit selected | ordered the unit where it stood instead of submitting the order |
| the target click with one unit selected | the same fork one step further along, and the one a player trips first |
| `GUI_Widget_RepairUpgrade_Click()` | started a repair on the clicking client only |

The last two are the ones worth remembering. Screen shake calls `sleepIdle()`
eight times from inside `Explosion_Tick()`, and `sleepIdle()` is where the match
pump lives — so a step re-entered the stepper and ran however many further ticks
the sleep happened to last. The guard against that was in the pump, which only
stopped the pump re-entering *itself*; it now lives in `MpGame_Step()`, which is
where the re-entry actually arrives. And the Starport window reseeds the
simulation's random generator so that its prices hold still while you look at
them — in a match, one player opening a window moved every random the other
player's game was about to draw. Both now use the interface generator.

### Saying so on the screen

Under the speed multiplier in the top right corner of the tactical view: `sync`
and the turn number in green while the two players agree, and `DESYNC t585: unit
map rng` in red, permanently, once they do not. A divergence used to announce
itself by the units behaving oddly, which is both late and ambiguous -- from the
player's chair a desync and an opponent playing badly look the same. The answer
already existed every turn; it only needed somewhere to be shown.

### The desync detector

Every turn packet carries a checksum of the state as of turn N, chunk by chunk,
and each client compares it with everyone else's. Nobody is the authority: a
mismatch is reported and the match stops being trustworthy from that moment,
because the loser's game would be corrupt either way.

```
mp-live: DESYNC at turn 6 (tick 48), about: map rng
```

`--mp-desync-dump` adds the evidence. The mismatch is always noticed `delay`
turns after the state it describes, by which time that state is gone — so with
the flag on, every turn is dumped and the dumps older than the delay deleted
again. When the two clients disagree about turn N, both still have their own
turn N on disk, and the answer is a diff:

```bash
./opendune ... --mp-relay=HOST,room,1 --mp-desync-dump
# mp-live: both players' turn 6 is in mpdesync-s*-turn6.bin
```

That is how the last three were found, each in one run: one differing byte in
one tile named the bloom on tile (52,31), and the explosion trace either side of
it named the sleep.

### What is still missing

* **The interface.** Done as far as starting a match goes — stage 5b. What is
  left is smaller and listed there: the waiting state is a frozen window, the
  lobby cannot show who else is in the room, and neither player picks a house
  independently of the other. The modal screens of §5 are done.
* **The production queue** is the last thing a player can do that is not a
  command. The Palace, the rally point, the Death Hand's aim and the Starport
  all travel now.

  The action panel is worth a second note, because the same fork bit twice. Its
  buttons and shortcuts run through `GUI_Widget_TextButton_Click()`, and its
  targeted orders finish with a click on the map in `GUI_Widget_Viewport_Click()`
  -- and *both* had a group road that submitted an order and a single-unit road
  that carried it out on the spot. Routing the button was not enough: pressing
  Move only decides what the next click means, and the order happens on the
  click. That second road is the one a player trips first, because pointing one
  unit at a tile is the most ordinary thing they do. Both roads submit now, in
  both places.

  The Starport now travels, and it needed one thing the others did not: a price.
  Its prices come from a generator seeded with the *viewer's* own house, so the
  two clients disagree what the buyer is being charged and no amount of routing
  fixes that by itself. The order carries the total it is paying, and both
  clients take that off the same house; the +/- buttons take nothing while the
  window is open, so a basket that is never bought costs nothing and cannot
  disagree. A client could of course name a price of its choosing -- so could it
  fabricate any other command, which is the standing bargain of lockstep between
  two people who chose to play each other.

* **`Map_FindLocationTile()`** is fixed. Case 5, "Visible", read the camera, and
  cases 4 to 7 applied their validity check only to the viewer's own house --
  so the search loop went round a different number of times on the two clients
  and every turn of it draws randoms. In a match "visible" means the owner's own
  base, and everybody is held to the same validity standard.

  The action panel deserves a note. Its buttons and their keyboard shortcuts run
  through `GUI_Widget_TextButton_Click()`, which had two roads out: a group went
  through `UnitSelection_BeginAction()` and became an order on the wire, and a
  *single* selected unit fell through to the original's code and was changed
  where it stood. `Unit_SetAction()` loads bytecode, so from that click the two
  clients ran different scripts for that unit -- the unit chunk first, then the
  tiles it walked over, then the randoms its script drew. Both roads now submit
  the same order. The deviation shake that the click did on the way past went
  with it, into the executor, for the same reason: it changes a unit.
* **`Map_FindLocationTile()`** ([map.c:965](src/map.c:965)) still reads
  `g_minimapPosition` in case 5 and gates validity on `g_playerHouseID` in cases
  4 to 7. It is reachable from reinforcements and from the AI, neither of which
  a two-human match uses yet.

## Stage 5b — the lobby (done)

Everything a match needed came off the command line. Two people who wanted to
play had to agree on a relay address, a room name, a seed, two houses and two
slots, and type all six of them correctly in two shells. The lobby is that
conversation moved into the game: **PLAY SOMEBODY**, the second row of the main
menu.

### The room name is the handshake

The one design decision. Whatever the two clients must agree on is folded into
the room name they ask the relay for:

```
    <game code>.<house pair>.<config digest>
```

The relay pairs two clients only when they ask for the same room, and it does
this without knowing why — it has never known anything about the game (see "The
relay knows nothing about the game", above). So a disagreement about the map, the
houses, the build or the balance ini cannot start a match. It surfaces as *the
other player never joined*, thirty seconds of waiting and a return to the menu.

That failure is worth the wait. The alternative — meeting first and comparing
afterwards — is a desync a few seconds into the match, which to a player is
indistinguishable from the game being broken. It also means there is no
handshake packet to write, no version negotiation, and no way for the check to
be skipped by a client that would rather not run it.

* The **game code** is the only thing anybody has to exchange, and it is the
  seed: `crc32(code) | 1`. Both sides derive the same map from it, so no seed
  travels. `| 1` because zero is a legal seed and far too easy to arrive at by
  accident — an empty code would otherwise silently mean a real map.
* The **config digest** is `crc32(revision + g_table_unitInfo + g_table_structureInfo)`.
  Hashing the two tables rather than the ini file is what makes it exact. The
  balance module and the unit tuning both work by patching those tables, so a
  key written out at its default value, a reordered file, a comment or a blank
  line changes nothing — and any difference that would actually change the
  simulation always changes the digest. The revision is in there because two
  builds with the same tables can still differ everywhere else.
* The **house pair** is one of the six *ordered* pairs of three houses. Ordered,
  not combinations, because slot 1 takes the first and slot 2 the second:
  "Atreides against Harkonnen" and "Harkonnen against Atreides" are one match
  seen from two chairs, and the row has to be able to say which chair you are
  in. The pair is in the room name; the slot deliberately is not — the two
  players are in the same room, they just sit in different seats.

### What it starts

`MpGame_TakeLobbyChoice()` ([opendune.c](src/opendune.c)) fills in the same four
statics `--mp-relay` fills in from the command line, sets both slots to
`MATCH_CONTROLLER_HUMAN_LOCAL`, and returns `GM_SKIRMISH`. From there the two
roads are one road: `MpGame_Begin()` connects, waits, and the loop of stage 6
runs the match. Nothing downstream of the lobby knows the lobby exists.

Both controllers are human by construction — a lobby match is 1-v-1, and a house
left on the AI is an opponent neither player agreed to.

### What shipped broken, and what the test should have said

The first version of this could never have worked, and the reason is worth
keeping: `Lobby_ConfigHash()` took a CRC over the raw bytes of the two tables.
`ObjectInfo` carries `const char *name` and `const char *wsa`, so those bytes
include two **addresses**, and a position-independent executable is loaded
somewhere different every launch. Two copies of the same build, on the same
machine, one second apart:

```
    client 1:  room dune42.0.227937b8
    client 2:  room dune42.0.b1efa065
```

Same code, same houses, same binary, different rooms. Each waited out its
timeout for a player who was in the other one, reported "the other player never
joined" to a console nobody was reading, and — see below — went round again.

Reaching round the two pointers fixed that, and left the deeper mistake
standing. The bytes *between* the fields belong to the compiler, not to the
game, and an arm64 build and an x86_64 build of the same commit still disagreed
— `74d90780` against `f2019082`, with the branch decoration already taken out of
the picture. So the digest is not taken over memory at all any more:
`Lobby_Fold()` folds each field as four little-endian bytes in an order this
file chooses, and only the values are the same on both machines.

One more thing was in there that should not have been. `g_opendune_revision` is
`g<sha>[M][-<branch>]`, and the branch is where the build happened rather than
what it is — the Intel package is built in a worktree, which is detached and has
no branch at all, so the two packages of one commit could not meet each other.
`Lobby_RevisionSpan()` cuts at the first dash. The `M` stays: a modified tree
really may be different code.

The self-test had a line about this and the line was the bug:

> The build digest has to be stable within one process, or two runs of the same
> binary would fail to meet each other.

Stable *within one process* is exactly what a pointer is. The claim that
actually matters is that the digest does not depend on where anything lives, and
that can be tested inside one process by moving something: repoint a table
entry's `name` at a copy of the same string and demand the digest not budge.
Restoring the raw hash now fails the test on that line.

The general lesson, since this fork will hash more things: **a digest that has to
agree between two machines cannot be taken over memory at all** — not over
addresses, and not over the padding and layout the compiler chose for the target
it was building for. Fold the values. And **a test that compares one process
with itself cannot see either mistake**: the pointer half was caught by moving a
pointer, the architecture half only by building both and comparing the two room
strings, which is now the last step of the Intel packaging procedure.

### Two more ways it did not fail gracefully

* **The wait was a frozen window.** Joining was a `msleep(5)` loop: no SDL events
  pumped, no frame drawn. macOS put a beachball over a black window for thirty
  seconds and every report of it was "the game hangs". It waits on `sleepIdle()`
  now — which is what runs `Video_Tick` and the input pump — with a notice on
  screen, ESC to give up, and the reason held for six seconds when it ends badly.
* **A failed match reopened the lobby, for ever.** `GameLoop_GameIntroAnimationMenu()`
  switches on the *previous* choice, and the lobby handed off to `GM_SKIRMISH`
  with `stringID` still `STR_LOBBY_MENU`. So a failure came back to the menu,
  re-entered the lobby immediately — over the screen `GM_SKIRMISH` had already
  cleared to black — waited thirty seconds, failed, and repeated. The handoff
  sets `stringID = STR_NULL` and `drawMenu = true` now, so a failure lands in the
  menu with the lobby's rows still filled in.

### The guard

`--lobby-self-test` tests the rule, not the drawing, because the rule is what
protects people: same choices produce an identical room string, any difference at
all produces a different one, the seed is stable and never zero, the digest does
not move when a pointer does and always moves for a real balance change, and
`host`, `host:port`, `host:` and `host:0` all split into what `MpNet_Connect()`
wants.

`--lobby-play=relay,code,pair,slot` covers the half a self-test cannot reach: it
enters the menu as though PLAY SOMEBODY had been clicked and takes the BEGIN
branch with those values, so two headless processes play the lobby's own road
from the menu to a running match. It is how the pointer bug was found, and it
fires once — a test that returns to a menu nobody is sitting at spins for ever.

```bash
tools/relay/relay -listen 127.0.0.1:31337 &
./opendune --lobby-play=127.0.0.1:31337,dune42,0,1 &
./opendune --lobby-play=127.0.0.1:31337,dune42,0,2 &
```

### What is still missing

* **The window has not been seen by a person.** Its logic is tested and the
  widened menu draws under the dummy video driver without crashing; nobody has
  looked at the lobby or clicked a row.
* **The waiting screen is a line of text.** It says what it is waiting for and
  takes ESC, which is enough not to look broken, but it cannot say whether the
  relay has anybody else in the room or how long is left.
* **The lobby cannot see the room.** The relay knows who is in it; the lobby
  never asks. It cannot show the other player's name, whether anybody is
  waiting, or which slot is taken — so two players who both pick "player 1"
  find out by not meeting.
* **Neither player picks a house independently.** They pick the same *pair* and
  take opposite ends of it, because the pair is in the room name. Letting each
  choose their own house needs a real config channel — that is, the handshake
  this design was built to avoid.
* **The strings are English literals**, like the modal message of §5. They do not
  go through the string table and do not translate.

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

## Release checklist

Everything that requires removal or a decision before shipping to players.

**`--mp-lag` flag.** Useful only during development to emulate bad network
conditions. Safe to leave in the binary (it does nothing without the flag), but
consider removing the argument parser entry so it does not appear in `--help`
output and confuse players.

**`--mp-units=N` starting squad.** Currently off by default with a comment "Off
by default until the squad stops desyncing." Decide before release: promote to a
lobby row (the count would go into the room name like everything else the two
must agree on) or remove the flag entirely.

**`debug_*` ini keys.** Upstream-inherited (`debug_game`, `debug_scenario`,
`debug_skip_dialogs`, `debug_log_game`). Present in the original OpenDUNE;
leaving them is fine. `debug_game` (control AI units) is the one most likely to
confuse players in a multiplayer lobby — it is purely local and the other client
will not see the resulting commands.

**`--sim-purity`.** A 40% overhead cross-check that checksums world state around
every drawing and input call. Keep in the binary for debugging, do not expose in
any player-facing interface.

**Savegame load from Options during a match.** Done — see §5. Load, Save,
Restart and Pick a new house all answer "Not while a network game is running"
while `MpTurn_IsActive()`, so the red row in
[mp-actions.html](mp-actions.html) is closed. What is still open is the wording:
it is an English literal rather than a table string, like the "No more
scenarios!" hint it copies.

## Rejected

* **Authoritative server with state snapshots** — a delta protocol over EMC script
  state. See the top of this file.
* **Rollback / GGPO** — the latency it buys does not matter at 130 ms turns.
* **Peer-to-peer without a relay** — NAT.
* **Letting one client's state win a checksum mismatch** — it hides the bug and
  can corrupt the other player's game.
