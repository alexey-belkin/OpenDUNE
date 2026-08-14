# Match telemetry — recording a battle and reading it back

Every other tool in this fork compares strategies. This one records a single
match and says nothing about it: `--war-telemetry` plays one battle, samples it
on a fixed tick interval, and writes a CSV. `tools/telemetry_report.py` turns
everything recorded so far into `telemetry.html` — a match picker with a chart.

It exists because the sweeps in [war.md](war.md) answer *which* strategy wins and
are structurally incapable of showing *how*. Two of the findings there were first
noticed here and only afterwards confirmed by measurement: that power surplus
goes negative before any structure falls, and that credits piling up mean a house
has hit the unit cap rather than got rich.

## Recording

```bash
cd bin
./opendune --skirmish=ordos,atreides --war=0,0,8919 --war-telemetry=200000,5000
```

| Flag | Meaning |
|---|---|
| `--skirmish=a,b` | which Houses play; without it, Atreides against Harkonnen |
| `--war=earlyA,earlyB[,seed]` | the early military share of each side, and the map seed |
| `--war-telemetry=ticks,step[,seed]` | how long to play and how often to sample |

The late share is fixed at 90% and the switch tick at 30000 — the tuned default.
Everything is written to `telemetry/` relative to the working directory, so
running from `bin/` puts recordings in `bin/telemetry/`. The directory is created
on demand and is gitignored; the generated `telemetry.html` is not.

Filenames carry enough to identify a recording without opening it:

```
telemetry/match-Ordos-Atreides-0v60-s32676-1786708585.csv
             House A  House B  shares  seed  unix time
```

## The file

Comment lines before the data, one trailing block after it, plain CSV in between.
The trailing block is trailing because the verdict is not known until the match
has been played, and rewinding the file to patch a header in would cost more than
a reader costs to skip to the end.

```
# recorded Fri Aug 14 14:56:25 2026
# houses Ordos,Atreides
# shares 0,60
# shareLate 90,90
# switchTick 30000
# seed 32676
# ticks 200000
# step 5000
tick,house,refineries,combatStructures,harvesters,combatUnits,combatHitpoints,damageTaken,spiceRefined,credits,powerSurplus
0,Ordos,0,0,0,0,0,0,0,1480,0
0,Atreides,0,0,0,0,0,0,0,1480,0
5000,Ordos,1,0,1,0,0,0,0,543,70
...
# winner Ordos
# value 37869,0
# wipeout 0,1
```

One row per house per sampled tick, in the order the houses appear in
`# houses`. The columns:

| Column | Kind | What it counts |
|---|---|---|
| `tick` | — | game tick of the sample; `step` apart |
| `house` | — | House name, matching one in `# houses` |
| `refineries` | level | standing Refineries |
| `combatStructures` | level | standing Barracks, WOR, Turret, Rocket Turret, Repair, IX, Palace |
| `harvesters` | level | Harvesters, **including ones inside a refinery** |
| `combatUnits` | level | units that are neither Harvester, Carryall nor MCV, and carry the `priority` flag — projectiles are units in this engine and are excluded by that flag |
| `combatHitpoints` | level | hitpoints summed over those units |
| `damageTaken` | **cumulative** | hitpoints knocked off this House's own units and structures |
| `spiceRefined` | **cumulative** | gross spice unloaded at a refinery |
| `credits` | level | credits on hand |
| `powerSurplus` | level | `powerProduction - powerUsage`; **signed**, and negative is the interesting case |

Two of them are cumulative on purpose. A rate is the difference between two
samples, and differencing a running total is exact, while sampling a rate
directly would miss whatever happened between two samples.

### Damage is recorded as taken, not dealt

Neither `Unit_Damage()` nor `Structure_Damage()` is told who fired — the engine
simply applies hitpoints. So the counter hangs off the *victim*, and "damage A
dealt" is read as "damage B took". In a two-house match that is the same number,
with one caveat: Palace specials land in the same column, so Fremen summoned by
an Atreides Palace and a Harkonnen Death Hand both count as their summoner's
output. That is fair enough — see the Palace section of [war.md](war.md).

### The verdict

`# winner` follows the same rule the search uses, in this order:

1. one side has no structures left and the other does — that side wins, and
   `# wipeout` marks which;
2. otherwise the match ran out of ticks: whoever holds more than 20% more asset
   value wins, where value is everything still owned priced at build cost scaled
   by remaining hitpoints;
3. otherwise `draw`.

## Building the page

```bash
python3 tools/telemetry_report.py [telemetry-dir] [output.html]
```

Defaults to `bin/telemetry` and `telemetry.html`. It reads every `.csv` in the
directory, sorts newest first, and writes one self-contained page.

**It bakes the data in rather than loading it on demand, and that is not
laziness.** A page opened from the filesystem can neither list a directory nor
`fetch()` a sibling file — browsers block both — so a picker that reads the
folder at view time cannot exist without a web server. The cost is that the page
is stale until regenerated. Re-run the script after recording.

The template is `tools/telemetry_report.html`, with `__MATCHES__` and
`__GENERATED__` substituted. Editing the page means editing the template, not the
generated file.

Two things the generator derives rather than reads:

* `damageDealt` — the per-step difference of the *other* house's `damageTaken`;
* `spiceDelta` — the per-step difference of the house's own `spiceRefined`.

A recording missing a sample for one house at some tick is dropped with a note
rather than plotted with a hole in it; that is what a file from an interrupted
run looks like.

## Adding a metric

Four places, in this order:

1. **`Skirmish_GetTelemetry()`** in [src/skirmish.c](src/skirmish.c) — compute it
   and append to the `snprintf`. If it counts units, remember that `Unit_Find()`
   hides units that are not on the map: a harvester unloading inside a refinery
   is one, and counting only the visible ones is a bug this project has already
   made twice. Set `g_validateStrictIfZero = 1` around the walk and restore it.
2. **The header line** in `WarSearch_RunTelemetry()` in
   [src/warsearch.c](src/warsearch.c) — the column names, in the same order.
3. **`COLUMNS`** in `tools/telemetry_report.py` if it is a level. A derived
   series goes next to `damageDealt` in the series builder instead.
4. **`METRICS`** in `tools/telemetry_report.html` — the label, and a formatter if
   the number wants thousands separators.

Old recordings keep working: the reader takes column positions from each file's
own header line, so a file written before the new column simply lacks it. The
page will fail to plot that series for those matches — the honest fix is to
re-record, not to fill in zeros.

## What a healthy match looks like

Useful as a baseline when something looks wrong:

* combat structures stay at zero for both sides until the switch tick — that is
  the default schedule working;
* combat units then rise almost vertically, because the share is a share of
  *accumulated* income and by the switch there is a lot of it;
* damage arrives in waves, not as steady pressure — teams fill to their minimum
  size and leave together;
* credits sit near zero while a house is healthy, and only pile up once it has
  run into the unit cap ([units.md](units.md));
* power surplus turning negative is the earliest sign of a collapse, well before
  structure counts move.
