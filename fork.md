# This fork of OpenDUNE — what it is, as of 13 September 2026

Upstream OpenDUNE is a faithful re-creation of Westwood's **Dune II**: the same
game, running natively. This fork keeps that engine and asks a different
question — *what would Dune II be if it had the conveniences of a modern RTS, an
opponent worth playing, and a second person on the other side of the internet?*

It starts at upstream `60019e87` (20 March 2026) and, at the time of writing,
adds **152 commits** across **107 files**, about 41500 lines added against 2000
removed, made between 11 August and 13 September 2026. Work up to 15 August is
on `master`; everything since is on `multiplayer`, 68 commits ahead of it.
`origin` is a private repository; the public upstream is read-only and is never
pushed to.

The day-to-day working notes are [CLAUDE.md](CLAUDE.md) — how to build, how to
verify, and the reasoning behind each rule. This document is the map above it:
what exists, why, and what comes next.

## The one thing that shapes everything

**The engine is ours; the game logic is not.** Unit, structure and team
behaviour lives in Westwood's `UNIT.EMC` / `BUILD.EMC` / `TEAM.EMC` bytecode
inside `DUNE.PAK`, and OpenDUNE interprets it. `Unit_SetAction()` loads
bytecode; it does not call a C function. Everything below is therefore either a
change to the engine around the scripts, or C that supplements a script between
its ticks — and the discipline that makes the second kind safe (who owns which
field, and when) is [emc-scripts.md](emc-scripts.md). Read it before touching
anything behavioural.

## What was done

### 1. A modern hand on the controls

Group selection anchored to the ground, finished on mouse release; selection
that survives an order; context-sensitive right-click orders and orders issued
from the minimap; edge scrolling; a repeat production queue at every factory
**and at the Construction Yard**, where the engine only ever allowed one
finished building to be held; a "place it" button of its own; the full-screen
build list as a grid that shows every item at once instead of four of a
scrolling strip; health bars and a unit status panel; a Repair All switch on the
Repair facility; a double-click window measured in wall-clock milliseconds, so
it does not shrink as the game speed rises.

Two keyboard bugs older than the fork were fixed on the way: `;` and `:` typed
`,` and `.`, and the left Shift did nothing at all.

### 2. Units that behave like an army

An **autonomy layer** on top of the original guard scripts: a unit has a *post*,
answers for the ground around it, goes out to meet what comes inside that area
and comes home afterwards. Player Attack orders pick a **tactical firing
position** by arrival time rather than driving into the target. Harvesters got a
recovery layer for the many ways the original script strands them, plus airlift
booking. The deviation budget was made a budget rather than a coin flip. Guard
posts follow a player's orders and not a script's incidental moves.

The router is **A\*** over the 64x64 grid with a cost function that reproduces
the movement layer — terrain rates, the speed quantisation, the movement tick —
so it optimises time and not an affine proxy for it, and prefers concrete
because concrete really is faster. A one-octant turn taken in motion no longer
stops the unit.

As of this month the computer's units get the same guard layer the player's have
(`skirmish_ai_guard`), which is what stopped "I can walk up and kill its guards
one at a time and the rest never move".

### 3. Rules as configuration, not as source

A tuning module reads `opendune.ini` at start-up and patches the tables:
a class-versus-class combat balance matrix with House identity bonuses,
per-unit damage and rate of fire, and a **tech tree** (`stock` or the fork's
`mp`, plus per-building overrides). Beside them sit the rules that are not table
patches — `pathfinder_astar`, `move_rolling_turn`, `build_slab_on_sand`,
`skirmish_base_rock`, `skirmish_ai_paving`, `skirmish_ai_guard`,
`starport_special_units`, `mp_start_units`.

Concrete may be laid on sand, which makes it a road and makes a base grow
wherever it lands; a small slab takes a quarter of a large one's time, as it
always should have. The Starport sells what a house could have built for itself
and is no longer a House of IX nobody had to build.

`python3 tools/threat_report.py` scores the roster against whatever the
configuration currently is, and `tools/threat_attrition.py` plays the fights out
shot by shot when the closed form is not enough.

### 4. An opponent, and a bench to judge it on

`--skirmish` puts two AI houses on a generated 62x62 map. Each starts with a
Construction Yard and a *plan* it works through: a build order compiled against
the tech tree that is actually standing, with power bought when it is needed.
Above that sit **doctrines** — A, the legacy engine-team behaviour kept as a
baseline, and B, which sorts an army into roles, musters it, takes it to a line
of departure and sends it in as a wave, keeping out of turret envelopes on the
way.

Around the AI there is a measuring apparatus, and it is most of the value:

* `--economy-search` evolves the fastest economic opening — [economy.md](economy.md);
* `--war-matrix` / `--war-timing` vary how much of the take goes to the army and
  answer with a win matrix — [war.md](war.md);
* `--war-metrics` is the regression suite: six maps, twice, sixteen numbers,
  each with a gate and a goal — [metrics.md](metrics.md);
* `--war-telemetry` records a match as CSV and `tools/telemetry_report.py` bakes
  it into a page — [telemetry.md](telemetry.md);
* [instruments.html](instruments.html) lists every measuring surface in one
  place, and [skirmish.md](skirmish.md) explains the sandbox itself.

The suite was **re-baselined on 13 September 2026** against the AI as shipped —
concrete paid for in time, guards that answer — rather than against the faster,
more passive AI it was first calibrated on, so no comparison is ever made
against an opponent nobody plays.

### 5. Two people, one match, over the internet

Deterministic lockstep, built in stages and documented in [mp.md](mp.md): the
clock taken off the wall, the RNG streams split between simulation and
presentation, `g_playerHouseID` demoted from a simulation input to a viewing
choice, a command layer, a turn loop with a delay derived from the game speed, a
Go relay, and a desync detector that names the savegame chunk two clients
disagreed about. `tools/mpdesync_diff.py` reads the resulting dumps in words.

Then the things that only a real network teaches: a turn too large for the wire
was being truncated in silence; a lost link ended the match instead of waiting;
a silent path was trusted for minutes because TCP does not report it; the client
was interrupting its own `connect()` sixteen milliseconds in and blaming the
relay. Every line a live match prints now also goes to `mp-live.log` on both
machines, which is the evidence when a match breaks.

**PLAY SOMEBODY** in the main menu settles a relay, a game code, a seat and a
House pair — and everything the two players must agree about is folded into the
room name, so a disagreement shows up as *nobody joined* rather than as a desync
a minute later. The first row of that lobby also seats **the computer** in the
other chair, which is how a person plays the skirmish AI.

Each player commands their own army and nobody else's, locally and
authoritatively.

### 6. Shipping it

`tools/package.sh` produces a macOS app another machine can actually open:
game data back inside the bundle, SDL vendored and the load command rewritten,
signed ad-hoc, the builder's effective ini carried along, and then **the archive
itself verified** — unpacked into a scratch directory and put through nineteen
self-tests before it is allowed to exist. An Intel package is a separate build
from a worktree, and the last step is always to check that both binaries derive
the same room digest, because an Intel player and an Apple Silicon player who
disagree simply never meet.

## How any of this is known to work

There is no test suite in the usual sense. Instead:

* **Self-tests that play the game.** Twenty-odd flags, each headless, each
  exercising one rule on a real match rather than asserting about a mock.
* **Verified to fail.** A test is not finished until the rule it guards has been
  deliberately broken and the test has failed *by name* — the phrasing "verified
  to fail: six deliberate breaks produce six different named failures" appears
  throughout [CLAUDE.md](CLAUDE.md) because a test that passes with the code
  removed is worse than no test.
* **Gates, not opinions.** Behavioural changes are measured on `--war-metrics`
  before and after, and the numbers are written into the commit message.
* **Determinism as a tool.** `--mp-checksum`, `--mp-replay`, `--mp-viewpoint`
  and `--mp-modal` all exist to make a difference between two runs impossible to
  ignore.

## Where it stands

Playable, both against the computer and against a person over a relay. The parts
that are known to be weak, stated plainly:

* **Doctrine B loses to doctrine A** on the AI as it now ships (`result.points`
  41, `result.wipeouts` 50), and B's harvesters die early. The re-baseline made
  that visible rather than causing it; the goals in [metrics.md](metrics.md) say
  where it has to get to.
* **Only the lobby's logic is verified.** Its rules and handoff have tests, and
  the window draws headless, but very little of it has been used by a person.
* **Matches outside a local network lag**, and nobody has yet profiled one.
* **No fog of war**, so both players see everything.
* Sixteen suite numbers, twelve still short of their goal.

## Future development

### The AI's strategies

A strategy is a straight line today: one military share, one army mix, one build
order, all fixed at t0. The plan is a **strategy profile** — a table of phases,
each with an entry condition and knobs along five axes (economy against war,
attack against defence, army against technology, the unit mix, and what the
attacks are actually for) — chosen in the lobby and searched for on the existing
benches, together with the instruments needed to judge one: recording any match,
compact text snapshots of the world, and metrics developed offline over
recordings instead of in C.

→ **[ai-strategy.md](ai-strategy.md)** — the full plan (draft).

### The match on the internet

Lag is reported outside a local network and has not been measured. The pieces to
do it with are already in place: the turn delay is derived from the speed
(`MpGame_DelayForSpeed()`), the relay's own `-lag` emulates a ping, the live log
records `turn N came after a 24827 ms wait`, and both clients plus the relay's
journal can be laid side by side. What is missing is the aggregation and the
verdict: whether it is the delay calibration, the relay, or the path — and then
the fix, which is probably an adaptive delay rather than a constant derived from
the speed alone.

### Commanding an army

* **Aircraft, aimed through the radar.** Ornithopters and Carryalls are
  `MOVEMENT_WINGER` and are excluded from the selection layer entirely, so a
  player cannot use them. They need to be selectable and orderable, and the
  natural aiming surface for a strike across the map is the radar — which
  already takes a right click as an order for ground units.
* **Escorting harvesters.** A "follow and guard" order: a unit takes another
  unit as its post and keeps station on it wherever it goes. The guard-post
  model already carries a post and a return; this makes the post a unit instead
  of a tile. It is also what the AI needs — `harv.exposure %` is the number it
  would move — so the same mechanism serves both sides.
* **Attacking enemy concrete.** An order onto an enemy slab is a move today.
  It should be an attack: concrete is a purchase and a road, and denying it
  ought to be possible without taking the building that stands on it.
* **A\* that knows where everyone will be.** The router treats a moving unit as
  traffic at double cost, which is a good approximation and not a plan. The next
  step is to reserve the *future* positions of allied moving units so two units
  do not route into the same tile at the same tick, and to make a column follow
  nose to tail rather than fan out and re-converge.

### Concrete as territory

Three rules that together turn paving from decoration into the shape of a base,
and each of which is a real change to how a base is attacked:

* **Connectivity.** Concrete that loses its path back to a source — the
  Construction Yard, a Windtrap — through other slabs and buildings goes
  neutral. `Tile.houseID` already holds three bits of ownership per tile, and
  `Structure_ConnectWall()` is the precedent for a connectivity pass over the
  map.
* **Capture at a finite speed.** Neutral concrete reconnected to a source is
  taken back, spreading at about the speed of a tank rather than instantly, so
  cutting a base in half is worth doing and reconnecting it takes time.
* **Power by contact.** A building draws power only if it touches something that
  produces it, through its own concrete or another building. Today
  `House_CalculatePowerAndCredit()` sums a house's production and usage
  globally, wherever the buildings stand. With contact required, layout matters
  and a severed base browns out.

### The map

* **Marks on your own concrete.** A coloured pixel a player can place on a slab
  they own — where a building is going, where an ally should stand. Cheap: the
  ownership bits and the tile drawing are both already there.
* **Spice generation as a match setting.** This fork regrows spice in a skirmish
  because two AIs mining continuously strip a map bare, which the original game
  never had to handle. It should be a switch both players agree on, and
  therefore folded into the room digest like every other rule.
* **Fog of war, as desert dust.** `Tile.isUnveiled` is one bit per tile, which
  means fog exists for exactly one house — see [mp.md](mp.md), §2. A match needs
  `unveiledByHouses`, one byte per tile, 4096 bytes in total plus a savegame
  field. It is its own stage and a prerequisite for nothing else, which is why
  it has waited.
