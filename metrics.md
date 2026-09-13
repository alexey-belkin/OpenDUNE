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

Readings are for doctrine B against doctrine A, on the AI as it is shipped
(see "Re-baselined" below), taken 13 Sep 2026 at the commit that made the
AI read its tech tree.

| metric | now | gate | goal | what it catches |
|---|---:|---:|---:|---|
| `turret.entries/match` | 11 | ≤24 | ≤8 | crossings into a turret's reach by anyone not in an assault |
| `turret.dwell/match` | 3308 | ≤13000 | ≤400 | *time* spent inside one — a fence units walk through and sit behind scores the same entries and far more dwell |
| `turret.deaths.loose` | 0 | 0 | 0 | died to a turret outside a wave in the assault. The rule, exactly |
| `turret.trade %` | — | ≥100 | ≥100 | turrets killed against units a turret killed during an assault. 999 means no assault deaths at all |
| `harv.lost.early` | 18 | ≤24 | 0 | harvesters lost before t100000, over the whole battery |
| `harv.lost/match` | 6 | ≤8 | 0 | harvesters lost, all match |
| `harv.exposure %` | 16 | ≤20 | ≤5 | samples with a harvester within eight tiles of something that shoots — the cause, which moves before the loss does |
| `harv.killed/match` | 9 | ≥6 | ≥10 | enemy harvesters killed: whether the raiders are actually hunting |
| `wave.launched/match` | 3 | ≥2 | ≥3 | that assaults happen at all |
| `wave.declined/match` | 15 | ≤60 | ≤20 | the force test refusing to go in. High means the wave is shuttling |
| `wave.cohesion %` | 75 | ≥70 | ≥85 | attackers on the wave against attackers in existence, sampled during an assault. "They do not all go together" is this number |
| `wave.first.tick` | 90172 | ≤130000 | ≤90000 | when the first assault begins |
| `wave.matches %` | 58 | ≥40 | ≥90 | share of matches with any assault at all |
| `econ.spice/match` | 58398 | ≥45000 | ≥65000 | the economy underneath, so a battle change that starves it is visible |
| `result.points %` | 41 | ≥30 | ≥75 | 2 per win, 1 per draw, against the baseline doctrine |
| `result.wipeouts %` | 50 | ≤60 | ≤10 | matches this side lost its last building |

## Re-baselined

The suite was first calibrated on an AI that laid its concrete for free and
whose guards stood still, and every reading recorded before 13 Sep 2026 was
taken against that AI: `turret.entries` 16, `harv.lost.early` 3,
`wave.matches` 91, `econ.spice` 66826, `result.points` 83,
`result.wipeouts` 8 at the file's first commit, and 79621 / 83 / 16 on the
last commit before the re-baseline. Two rules changed what the AI *is*:
`skirmish_ai_paving` (the yard pours a building's slabs at a slab's
buildTime a tile) and `skirmish_ai_guard` (a unit standing guard goes out to
meet what comes inside its area and returns to its post). Both are on, for
both houses, in what a person plays against, and the gates were re-taken on
that AI so that a comparison is never against a faster, more passive
opponent than anyone will meet -- `--ai-guard=0 --ai-paving=0` still gives
the older AI back, bit for bit, for reading the historical numbers.

The re-baseline is honest and it is not flattering: under the shipped AI,
doctrine B loses to doctrine A more often than not (`result.points` 41,
`result.wipeouts` 50) and its harvesters die early (`harv.lost.early` 18
against 3). The two AI rules hurt B more than A because B's reserve is meant
to accumulate at the muster and strike as one, and an opponent whose guards
answer, on a clock where production is slow, spends that reserve a unit at
a time; and B's harvesters have no escort while A's hunters now arrive with
factories that no longer wait behind an Outpost. Both are B's problems to
fix, and the goals column still says where the fixes have to take it. The
one change measured on this baseline so far, the AI reading its tech tree
for its build order, moved `econ.spice` 48703 → 58398, `wave.matches` 41 →
58, `wave.launched` 1 → 3 and `result.wipeouts` 58 → 50, and left
`result.points` at 41.

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
