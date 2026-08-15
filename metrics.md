# The metrics suite — a doctrine's standing orders, as numbers

```bash
cd bin && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune \
  --skirmish=ordos,harkonnen --doctrine=B,A --war-metrics=200000,6
```

Six maps, each played twice with the doctrines swapped between Houses, about
forty seconds. Run it before and after every behavioural change.

## Why it exists

Every fix in the doctrine layer so far has broken something that was already
fixed. The turret fence stopped units leaving; the flank route stopped them
fighting; a rule written for B leaked into A and A was seen raiding harvesters.
All of them were found by eye, matches later, rather than by the run that caused
them. A battery that always plays the same maps and prints the same sixteen
numbers turns "it looks worse" into a diff.

Both sides are measured. The leak into A was invisible precisely because nobody
was reading A's column, and a baseline that drifts is not a baseline.

## Gate and goal

Two thresholds per metric, because they answer different questions.

* **goal** — where the behaviour should end up. Several are nowhere near met.
* **gate** — where it stands now, with room for noise. Breaking one is a
  regression: something that worked this morning does not any more.

Only a broken gate is a failure. A suite carrying goals alone would print FAIL
for ever and be ignored; one carrying gates alone would bless whatever it
happened to measure first.

## The sixteen

Readings are for doctrine B against doctrine A, at the commit that introduced
this file.

| metric | now | gate | goal | what it catches |
|---|---:|---:|---:|---|
| `turret.entries/match` | 16 | ≤24 | ≤8 | crossings into a turret's reach by anyone not in an assault |
| `turret.dwell/match` | 8684 | ≤13000 | ≤400 | *time* spent inside one — a fence units walk through and sit behind scores the same entries and far more dwell |
| `turret.deaths.loose` | 0 | 0 | 0 | died to a turret outside a wave in the assault. The rule, exactly |
| `turret.trade %` | — | ≥100 | ≥100 | turrets killed against units a turret killed during an assault. 999 means no assault deaths at all |
| `harv.lost.early` | 3 | ≤6 | 0 | harvesters lost before t100000, over the whole battery |
| `harv.lost/match` | 0 | ≤2 | 0 | harvesters lost, all match |
| `harv.exposure %` | 8 | ≤12 | ≤5 | samples with a harvester within eight tiles of something that shoots — the cause, which moves before the loss does |
| `harv.killed/match` | 12 | ≥6 | ≥10 | enemy harvesters killed: whether the raiders are actually hunting |
| `wave.launched/match` | 3 | ≥2 | ≥3 | that assaults happen at all |
| `wave.declined/match` | 39 | ≤60 | ≤20 | the force test refusing to go in. High means the wave is shuttling |
| `wave.cohesion %` | 79 | ≥70 | ≥85 | attackers on the wave against attackers in existence, sampled during an assault. "They do not all go together" is this number |
| `wave.first.tick` | 110016 | ≤130000 | ≤90000 | when the first assault begins |
| `wave.matches %` | 91 | ≥60 | ≥90 | share of matches with any assault at all |
| `econ.spice/match` | 66826 | ≥50000 | ≥65000 | the economy underneath, so a battle change that starves it is visible |
| `result.points %` | 83 | ≥55 | ≥75 | 2 per win, 1 per draw, against the baseline doctrine |
| `result.wipeouts %` | 8 | ≤25 | ≤10 | matches this side lost its last building |

The baseline column is blank-ish for the wave and turret rows: doctrine A has no
waves and does not run the fence, so those counters are structurally zero for it.
Its harvester and result columns are real and are the ones to watch for leaks.

## Two things it had to be given first

**Determinism.** `Timer_SetTimer(TIMER_GAME, false)` used to be called *after*
`Skirmish_StartWar()`. Building a match takes real time, so however many timer
ticks landed inside that window was a property of machine load; the match then
began at a different absolute tick every run and everything the engine gates on
tick parity fell differently. Two runs of the same binary disagreed by a third on
spice. The clock now goes off first, here and in `WarSearch_Match()`.

**A match clock.** `g_timerGame` runs for the life of the process and a search
plays hundreds of matches inside one, so everything phrased as "before tick
100000" was true of the first match and of no other. Resetting the global was
tried and broke the economy outright — too much of the engine stamps absolute
ticks — so `doctrine.c` keeps `s_matchStart` at reset and measures its deadlines
against it.

A third, found on the way: `Unit_Damage()` books a kill for every shot that lands
on something already at zero hitpoints, and a wreck lying under a turret booked
over a thousand harvester losses in one match. Counted once per unit now, on the
hitpoints as they were on the way in.

## Adding a metric

1. A counter in `doctrine.c`, cleared in `Doctrine_Reset()`. If it is a *state*
   rather than an event, sample it in `Doctrine_SampleMetrics()` — exposure and
   cohesion are both there, because by the time either shows up in a loss the
   match is over.
2. A field in `DoctrineMetrics` and a line in `Doctrine_GetMetrics()`.
3. Sum it in `WarSearch_MetricAdd()` and add a `WarSearch_MetricRow()` call.
4. Set the gate from a measured run, the goal from what the rule actually asks.
