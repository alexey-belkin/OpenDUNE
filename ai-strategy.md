# AI strategy: parameterised, and the instruments to judge it

**Draft, 13 September 2026. Nothing here is built.** It is the plan agreed after
the AI was given its concrete in time, guards that answer for their ground and a
build order that reads the tech tree — three fixes that each came from one
sentence of a person's report, and each of which took a day of measurement to
land safely. The point of this document is to stop that being the process: to
make a strategy a set of numbers that can be written down, searched for and
compared, and to make a match something that can be read back after the fact
instead of watched.

## 1. What is parameterised today, and where it runs out

Two structures hold everything the AI can be told.

`SkirmishEconomyPlan` ([skirmish.h](src/skirmish.h)) is the *economic* half: an
ordered list of buildings, how many harvesters and carryalls to build towards,
how patient to be before adding a Refinery or a Carryall, and the war ledger —
`militaryShare`, `militaryShareLate`, `militarySwitchTick`, the percentage of
lifetime income the house may spend on war before and after one switch.

`DoctrineParams` ([doctrine.c](src/doctrine.c)) is the *military* half: wave
size, the minimum worth attacking with, the garrison kept at home, the line of
departure standoff, the column hold and release, the abort and release
percentages, the assault ratio, the picket share, and `roleShare[4]` — the army
composition, over four roles (artillery, assault, raid, garrison).

Between them they can express a great deal. What they cannot express is
**change**: one military share until a single switch tick and one after it, one
army mix for the whole match, one build order compiled at t0. A house cannot
rush and then boom, cannot build Trikes until it has a Heavy Factory and tanks
afterwards, cannot decide that it is losing and go defensive. Every strategy
this fork has measured is a straight line drawn at the start of the match.

The second gap is that there is no way to *choose* between strategies from
outside the source. `--doctrine=A,B` picks one of two compiled-in battle
doctrines; the economy plan comes from `WarSearch_MakePlan()` with a share and a
switch tick. A person in the lobby cannot ask for a rushing opponent.

## 2. The shape: a profile is a table of phases

One object, the **strategy profile**, read from `opendune.ini` under its own
heading (`[strategy.rush]`, `[strategy.develop]`, `[strategy.war]`) and named in
the lobby beside the House pair. A profile is an ordered list of **phases**;
each phase is an entry condition and a set of knobs that hold while it is
current. The house is always in exactly one phase, and re-evaluates on the same
cadence the doctrine tick already runs on.

An entry condition is a conjunction of things the house can see about itself and
about what it has met:

| trigger | reads |
|---|---|
| `after_tick` | the match clock, `s_matchStart` relative |
| `harvesters_at_least` / `at_most` | own harvester count |
| `income_per_10k_below` / `above` | `s_harvested` over a window |
| `credits_above` | `h->credits` |
| `enemy_seen_within` | distance of the nearest enemy unit to the base |
| `under_attack_for` | ticks since the last hit on own structures |
| `lost_structures_at_least` | own losses this match |
| `tech_reached` | a building standing (`structuresBuilt`) |

The knobs a phase may set are the five axes below. Anything a phase does not
mention keeps the value of the phase before it, so a profile is written as a
baseline plus deltas rather than as three complete copies of every number.

This is deliberately not a behaviour tree or a scripting language. A phase table
is small enough to search over, and search is how every number in this fork that
survived contact with `--war-metrics` was found.

## 3. The five axes

### 3.1 Economy against war

The existing `militaryShare` becomes per-phase, and the harvester target with
it. Three named shapes fall straight out:

* **rush** — one harvester, share 90 from t0, no second Refinery, the build
  order stopping at the first factory. A profile may decline to have this phase
  at all: `rush.enabled=0` is a legitimate strategy and the search should be
  allowed to answer that it is the better one.
* **develop** — harvesters up to the plan's target, share 10 to 20, the tech
  buildings bought early because the money is there.
* **war** — harvesters replaced as they die plus slow growth, share 70 to 90,
  everything else into the army.

The knobs: `military_share`, `harvester_target`, `harvester_growth` (how much
above replacement), `refinery_wait`, `carryall_wait`.

The interesting question the phase table makes askable is not the level of the
share but **when** each phase starts, and that is exactly what `--war-timing`
already found mattered more than the level.

### 3.2 Attack against defence

Two things a house spends on that are not the attacking army: turrets, and the
units it does not send.

* `defence_share` — of the military budget, how much may go to the forward line.
  Today the turrets are simply entries in the build order, and they compete with
  units through `Skirmish_War_MilitaryAllowed()` on a first-come basis.
* `turret_target` per phase — how many turrets are wanted at all. A rush phase
  wants none.
* `garrison_keep` — already exists, becomes per-phase.
* `escort_count` / `escort_per_harvester` — units posted to the fields the
  harvesters work rather than to the base. The guard layer built for
  `skirmish_ai_guard` is the mechanism; a post on a spice field is the same
  thing as a post at home, and `harv.exposure %` is the number that would move.

### 3.3 Army against technology

Whether to spend on more of what the house can already build, or on the
buildings that unlock better. The plan compiler now knows the tech tree
(`Skirmish_Plan_EnsurePrerequisites()`), so a phase can name a **tech target**
by building or by unit — "reach the Siege Tank", "reach the House of IX" — and
the compiler works out what that costs in buildings.

* `tech_target` — a StructureType or UnitType, or none.
* `tech_share` — of the military budget, the share reserved for reaching it,
  so a house does not spend the whole of a rush on a tech building nor arrive at
  IX with no army.

### 3.4 The mix

`roleShare[4]` becomes per-phase and the roles are split finer, because "rush
with Trikes" and "rush with rocket infantry" are the same row today:

| role | units |
|---|---|
| infantry light | Soldier, Infantry |
| infantry rocket | Trooper, Troopers |
| light vehicle | Trike, Raider Trike, Quad |
| tank | Tank, Siege Tank, Devastator |
| artillery | Launcher, Sonic Tank (and the Saboteur as Ordos' stand-in) |
| air | Ornithopter, Carryall |

`Doctrine_RoleOf()` already sorts by type and would carry the finer split;
`Doctrine_PickUnit()` already builds towards a share vector. The air row waits on
aircraft being commandable at all (see [fork.md](fork.md), future work).

### 3.5 What the attacks are for

`Doctrine_PickObjective()` scores candidate objectives already; the weights
become a per-phase vector rather than a compiled-in ranking:

* enemy harvesters (the strangle),
* the enemy base, itemised — Construction Yard, Refineries, factories, power,
* the enemy army in the field, which is the defensive answer: kill what is
  coming before it reaches our harvesters.

A rush profile weighs harvesters and the yard; a war profile weighs factories;
a defensive phase weighs the army. The raid rule that exists today is the
harvester weight set high with everything else at zero.

## 4. Where the numbers come from

Not from opinion. Three benches exist and each takes one more argument:

* `--economy-search` evolves the economic half already. It gains the phase
  boundaries as genes.
* `--war-matrix` / `--war-timing` play tuned economies against each other at
  varying shares. They gain the profile as the thing being varied, and the
  output stays a win matrix rather than a number, because that is the honest
  shape of the answer.
* `--war-metrics` is the guard, re-baselined on 13 Sep 2026 against the AI as
  shipped ([metrics.md](metrics.md), "Re-baselined"). A profile is not accepted
  because it beat one opponent; it is accepted when the suite does not regress
  against the same six maps.

And one that does not exist yet: **a profile played against a recording of a
person**. Section 5 is what makes that possible.

## 5. The instruments

Three pieces, in this order, because none of the later ones can be judged
without the earlier ones.

### 5.1 Recording a match

The determinism work already gives us the hard part: a match is a seed plus a
configuration plus a stream of commands, and replaying that stream reproduces
the match bit for bit (`--mp-record` / `--mp-play`, `--mp-checksum`). What is
missing is that this only happens on the harness road. The recorder should be
switchable on for **any** match — a person against the computer from the lobby,
two people over the relay, an AI against an AI — writing into the personal data
directory beside `mp-live.log`.

A recording holds: a header (revision, config digest, seed, houses, controllers,
the rules that are not table patches), the command stream, and periodic
**snapshots** (5.2). The commands make it replayable; the snapshots make it
readable without replaying, and are the checkpoint a reader can seek to.

### 5.2 The snapshot

A compact text serialisation of the world at one tick — text because the reader
of last resort is a person or a model reading it in a terminal, and because a
diff of two of them should be legible.

Per house: credits, power produced and used, spice, what is standing (type,
tile, hitpoints, state, what it is making), the doctrine's own state (phase,
muster point, objective, wave membership and strength). Per unit: type, tile,
hitpoints, action, target, destination, role, whether on a wave, guard post.
Harvesters get their own line each, because that is where the money is.

Optional and off by default: the map layer — spice per tile, ownership, fog —
run-length encoded, which is the only part that is large.

Cadence: every 1000 ticks by default, about sixteen seconds at normal speed,
which puts a 200000-tick match at 200 snapshots. At an estimated 5 to 10 KB each
that is 1 to 2 MB per match, and the map layer roughly doubles it. Denser
sampling on a window (`--snapshot=100@60000-80000`) for a fight worth looking at
closely.

Two ways to get a tick that was not sampled:

* `--dump-at=TICK[,area]` replays the recording to that tick and writes one
  snapshot, optionally clipped to a rectangle. Exact, because the replay is
  deterministic.
* `--replay-shot=TICK` does the same and renders the viewport headless to a PPM,
  which is the "screenshot from a replay" — the machinery exists already for
  `--build-list-dump` and the viewport self-test.

### 5.3 Reading it back without the game

`tools/matchlab/`, Python, reading recordings and snapshots:

* print the state at a tick, or the difference between two ticks, as a table;
* follow one unit, one structure or one house across the whole match;
* run **metrics as plugins** — a metric is a function over a sequence of
  snapshots, added as a file, with no game rebuild and no rerun of the match.

That last point is the one that pays. The sixteen numbers of `--war-metrics`
live in C today, so a seventeenth costs a rebuild and forty seconds of matches
per experiment, and a metric that turns out to measure the wrong thing costs the
same again. Over recorded matches a candidate metric is a five-line function
tried in a second; only the ones that survive get written into C as gates.

The existing suite should be reproducible in `matchlab` over the same six seeds,
and that equivalence is the test of the whole pipeline: if the Python and the C
disagree about `harv.lost.early`, one of them is lying about what it counts.

## 6. Order of work

1. Recording and snapshots for any match, and `matchlab` reading them. Nothing
   else can be judged without this.
2. Metric plugins, and the sixteen reproduced over recordings.
3. The profile: the phase table, the ini parsing, the three presets, the lobby
   row, and the profile folded into the config digest — two players must not
   disagree about what the computer is.
4. The axes one at a time, each with its own measurement, in the order they are
   cheap: economy phases, mix, objectives, escorts, tech targets.
5. The searches extended to the profile, and the presets re-derived rather than
   hand-written.

## 7. Open questions

* **Does the profile belong in the digest?** A match against the computer is
  local, so strictly it need not be agreed. But a code that names a map ought to
  name the same opponent too, or two people cannot compare a match. Leaning
  towards folding it in.
* **How much does a snapshot cost in a live match?** Sampling walks both pools;
  at 1000 ticks that is nothing, but the writer must not be on the simulation's
  critical path in a networked match, where a hitch is a stall for the other
  player as well.
* **Do phases need hysteresis?** A trigger on income will oscillate around its
  threshold. The decline timer in `Doctrine_PhaseMuster()` is the precedent: a
  decision that has been made sticks for a while.
* **One profile per house or one per match?** Two AIs on one map with different
  profiles is the interesting case for the bench; a person in the lobby wants
  one row, not two.
