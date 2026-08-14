# Economy search — finding the fastest opening

The question: given a Construction Yard and 1500 credits, what gets spice out of
the ground fastest? Nothing in this module decides the answer. It runs the real
game headless, one opening per match, and scores what came out of the refinery.

The result is meant as the *economic ceiling*: combat is switched off here, so a
plan can only spend credits on its own growth. Military spending gets balanced
against that ceiling later.

## Running it

```bash
cd bin
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --economy-baseline=80000,3
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --economy-grid=80000,3
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --economy-search=30,25,80000,3
```

| Flag | Arguments | What it does |
|---|---|---|
| `--economy-baseline` | `ticks,maps` | scores three hand-written openings |
| `--economy-grid` | `ticks,maps` | sweeps refineries x harvesters x carryalls |
| `--economy-search` | `pop,generations,ticks,maps` | evolves an opening |
| `--economy-trace` | — | prints one line per 10000 ticks of every match |
| `--economy-queue` | `ticks,maps` | sweeps the adaptive refinery rule |
| `--economy-carryall` | `ticks,maps` | sweeps the adaptive carryall rule |
| `--economy-play` | `refineries,harvesters,carryalls[,refineryWait,carryallWait,seed]` | plays one plan in the GUI, to watch |

`--economy-play=1,32,0` is the surprising cell of the table below: one refinery,
thirty-two harvesters. The overlay leads with the refined total, so what is on
screen can be checked against what the search reported.

A match of 80000 ticks takes about 25 ms, so a 30x25x3 search (2250 matches)
runs in a minute. Each generation prints elapsed and remaining time.

## What a plan is

The genome holds economic *decisions* only:

* an ordered list of Refinery / Heavy Factory / Hi-Tech / Starport / Silo;
* how many harvesters and carryalls to keep;
* how many of each to order from the Starport.

Everything else is not a decision and is filled in by `EcoSearch_Compile()`:

* **prerequisites** — the Outpost and Light Factory are tolls on the way to the
  Heavy Factory, not alternatives to it;
* **windtraps** — one is emitted whenever the power already committed would go
  into deficit. A base short of power caps every structure at half hitpoints; a
  base with spare windtraps has thrown away 300 credits each.

Combat structures cannot appear at all, and `Skirmish_AI_AllowUnit()` refuses
anything but harvesters and carryalls, so no plan can quietly spend its economy
on soldiers.

## Fitness

Gross spice **unloaded at a refinery** within a fixed tick budget, averaged over
a fixed set of maps. The counter sits in `Script_Structure_RefineryHarvest()`
([src/script/structure.c:147](src/script/structure.c:147)), on the same line that
pays the house — spice scooped from the ground but still riding in a harvester's
tank does not count, and neither does a load lost on the way home. Gross rather
than credits on hand: credits get spent, and a plan that turned spice into
harvesters has not lost anything. The map seeds are fixed, so two
plans are always compared on the same ground, and the RNG is reseeded per
evaluation — the same genome always scores the same.

## What it found

Numbers below: 80000 ticks, 3 maps, spice refined.

Hand-written openings:

| Opening | Spice |
|---|---:|
| Refinery → Heavy Factory → Silo → Hi-Tech, 3 harvesters, 1 carryall | 9264 |
| three Refineries, 6 harvesters | 4784 |
| Refinery → Starport → Refinery, 4 harvesters, 3 bought | 4508 |

Those three numbers are the oldest measurements in this file and the code has
moved under them; today the same flags give 8131 / 741 / 2435. The two openings
that fell are the two with several refineries and no Heavy Factory, and they fall
because of the refinery handshake bug in [harvester.md](harvester.md) — with one
harvester and three refineries, one stuck door is the whole economy. They are
kept here as the bar the search cleared, not as current figures. `--economy-trace`
now prints the state of every harvester and every refinery, which is where to
start when an opening scores near zero.

Evolved best: **19225**, roughly twice the best hand-written opening.

```
Windtrap, Refinery, Outpost, Windtrap, Light Fctry, Heavy Fctry
harvesters 16, carryalls 0, starport 5h/7c
```

That is the *minimum* path to a Heavy Factory and then everything into
harvesters. The driver sweep at that horizon:

| Refineries | 2 harv | 4 harv | 8 harv | 16 harv | 32 harv |
|---:|---:|---:|---:|---:|---:|
| 1 | 9104 | 11747 | **17013** | 16433 | 17426 |
| 2 | 7336 | 11000 | 14284 | 17277 | 14057 |
| 3 | 8634 | 8798 | 12958 | 12590 | 13563 |
| 4 | 6642 | 7448 | 7864 | 9271 | 8538 |

* **Harvesters are the driver.** Going from 2 to 8 nearly doubles the take.
* **Carryalls currently hurt, badly.** Adding two costs a Hi-Tech factory and
  drops the take from 17013 to 7324 at eight harvesters — far more than the
  building itself is worth. Something in the airlift path is losing harvester
  time; this is the first thing to look at rather than a settled result.

## The horizon decides the answer, so do not fix it

The table above says one refinery wins. That is true *for 80000 ticks* and
misleading as a general result: at that point the house has only managed to
field three to six harvesters — the harvester target is a ceiling it never
reaches, not a fleet it has — and one refinery is idle most of the time. Run the
same sweep four times longer and it inverts:

| Refineries | 16 harv | 32 harv |
|---:|---:|---:|
| 1 | 123412 | 124398 |
| 2 | 134275 | **154037** |
| 3 | 143894 | 153895 |
| 4 | 135235 | 152433 |

Both regimes are real. A plan with a fixed refinery count has to bet on one of
them, and any bet is wrong for half the match.

## The adaptive rule

So do not count refineries — count the queue. Every 60 game ticks
`Skirmish_Economy_SampleQueue()` asks: is any refinery free, and is a harvester
sitting there with a load it cannot deliver? After N such samples in a row,
another refinery is added to the plan. N is part of the genome (`refineryWait`,
0 disables it).

Two details are what make it work rather than nearly work:

* **Its own cadence.** The first version sampled where the Construction Yard asks
  what to build next, which sounded like the natural place and turned out to fire
  four times in forty-five seconds — the yard is usually busy. A queue could sit
  there for a whole session unnoticed. It now runs from the house loop on a
  timer, whatever the yard is doing.
* **One refinery at a time.** A queue does not clear until the new refinery is
  standing, so a rule that keeps reacting to the same queue buries the economy in
  refineries it cannot pay for. Measured, at some thresholds, as a collapse from
  150000 refined spice to 1500. The rule now waits for the refinery it ordered.
* **Waiting is not only "full".** Insisting on a full 100 load missed most of the
  queue: a harvester sent home early by the recovery layer carries whatever it
  had. Anything with cargo that is heading home counts.

`--economy-queue=300000,1`, starting from one refinery:

| Harvester target | fixed (N=0) | N=1 | N=2 | N=4 | N=8 | N=12 |
|---:|---:|---:|---:|---:|---:|---:|
| 8 | 102946 | 117855 | **122794** | 117977 | 114599 | 120251 |
| 16 | 111611 | 145550 | 140779 | **158852** | 152500 | 149400 |

Every adaptive setting beats every fixed count, by 15-40%, which is the point of
a rule over a number. **N=2 to N=4** — one wasted trip can be bad luck, two or
three in a row is a pattern. Below that it over-reacts, above it the queue is
allowed to stand.

The rule still has no total cap: over 300000 ticks it puts up a dozen refineries,
limited only by the plan size. The per-refinery figures from `--economy-trace`
say the late ones do real work (3000-8000 refined each), so this is not yet
waste — but "stop when the newest refinery is barely used" remains the obvious
next refinement.

Watching it: `--economy-play=1,16,0,2` queues its second refinery around tick
11000, which is a minute or two of real time at the default speed. The overlay's
plan counter is the tell — `6/6` becomes `6/7` the moment the rule fires.

### Recalibrating N once carryalls exist

On three maps, with the carryall rule on (P=1200), 300000 ticks:

| Harvester target | N=0 | 2 | 5 | 8 | 10 | 12 | 15 | 20 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 | 84714 | 125181 | 124601 | 122788 | *79726* | 124511 | 124415 | 127467 |
| 32 | 82026 | 140559 | 136599 | 137011 | *86919* | 142339 | 144083 | *99099* |

Between 2 and 20 the result is flat within about 8% — N is not a lever, it is a
switch: having the rule at all is worth 50%, where you set it is worth noise. The
italic cells were reproducible collapses to roughly the no-rule level; N=10
failed in both rows on every run of this sweep.

**Those collapses are now explained and fixed.** They were the harvester
re-target livelock described in [harvester.md](harvester.md): a harvester that
switches refinery on every tick tears up its own route before it has moved a
tile, and stands in the doorway for the rest of the match. Which values of N
happened to produce the standoff was luck, which is why it looked like noise
without a pattern. With the cooldown in place the same sweep reads:

| Harvester target | N=0 | 2 | 5 | 8 | 10 | 12 | 15 | 20 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 | 64180 | 126720 | 130201 | 126420 | 127748 | 130613 | 129658 | 94621 |

Flat within 4% from N=2 to N=15 and no collapses. The conclusion is unchanged and
now rests on a clean measurement: **have the rule, and do not agonise over N.**

**Caveat on the whole refinery result** (raised by watching, not by measuring):
new refineries are placed in the next free slot of the base rectangle, which is
packed from the top left, while the spice tends to lie right and down. Part of
what the rule earns may simply be that later refineries stand closer to the
spice. Separating the two would mean placing a refinery by where the spice is
rather than by where the next slot is — worth doing, and worth doing before
trusting the numbers above as "capacity".

## Carryalls, on the same principle

The fixed-count measurement said carryalls were a disaster. They are not — buying
two up front is. The rule that works asks the same kind of question the refinery
rule does:

1. below four harvesters there is nothing for a carryall to do, so do not build
   one (and do not build the 500 credit Hi-Tech factory that makes it);
2. after that, add one whenever a loaded harvester has been driving home for more
   than P ticks **while a refinery stands free** — free refinery means this is
   road time, not queue time, and road time is exactly what a carryall removes.

The split matters: without it the two rules chase the same symptom and both fire.
The Hi-Tech factory, and a windtrap for it if the power is not there, are
appended to the running plan when the first carryall is justified.

`--economy-carryall=300000,1`, with the refinery rule fixed at N=5:

| Harvester target | off | P=300 | P=600 | P=1200 | P=2400 | P=4800 |
|---:|---:|---:|---:|---:|---:|---:|
| 8 | 118050 | **129702** | 119443 | 128569 | 127269 | 125992 |
| 16 | 134224 | 150371 | 153321 | **156120** | 150039 | 153703 |

Every threshold beats no carryalls at all, by 10-16%. **P = 1200** (twenty
seconds of game time) is the default; the spread between thresholds is within the
noise of a single map, so treat the exact number as "somewhere around a thousand
ticks" rather than a tuned constant.

## Caveats worth keeping in mind

* Three fixed maps is a small sample. A plan can still be tuned to them.
* The search converges early and then coasts — the generations after ~15 mostly
  refine scalars. Wider populations explore build order better than longer runs.
* Fitness is a fixed horizon, not "time to strip the map". A plan that is slow
  to start but scales better looks worse than it is over 80000 ticks — this bit
  once already, see the section above. Run any conclusion at two horizons.
* Evaluation is deterministic only because the game clock is taken away from the
  SDL timer thread for the duration (`Timer_SetTimer(TIMER_GAME, false)` and a
  manual `g_timerGame++`). Without that, how many real-time ticks land between
  two simulated ones varies with machine load and the same genome scores
  differently on consecutive runs.
