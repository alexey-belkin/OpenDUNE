# The war phase — how much of the economy should be an army?

The economy search ([economy.md](economy.md)) answered a question a single house
can answer alone: how fast can spice come out of the ground with nothing
shooting back. This asks the one that only exists once there is an enemy.

The answer cannot be a number in the same sense. Refined spice is something a
house maximises against the map; a spending split is only good or bad relative to
what the other house is spending. So nothing here scores a plan on its own. Two
strategies are put on one map, and the result is who was still standing.

## The split is a budget, not a build order

Of every credit a house has ever had — its 1500 of starting capital plus
everything it has refined since — at most `militaryShare` percent may end up in
soldiers, tanks and turrets. There is nothing else to gate: whatever the army
does not take has nowhere to go but the economy.

The ledger is charged in `Structure_Update()`
([src/structure.c:345](src/structure.c:345)), the one place in the engine where
production is paid for, credit by credit as a countdown runs. A half-finished
tank is half-charged, which is what makes the share behave like a spending *rate*
rather than a checkout gate.

What counts as military:

| Military | Economy |
|---|---|
| every combat unit | Harvester, Carryall, MCV |
| Barracks, WOR | Construction Yard, Windtrap, Refinery, Silo |
| Turret, Rocket Turret | Outpost, Light Fctry, Heavy Fctry, Hi-Tech, Starport |
| Repair Bay, House of IX, Palace | |

The Heavy Factory is on the economy side even though tanks come out of it,
because harvesters come out of it too and the house cannot have one without the
other. A Barracks is military because nothing but soldiers ever comes out of it —
and it is gated by the same budget, so a house running a low share walks straight
past the Barracks in its plan and builds the next refinery instead
(`Skirmish_Plan_PickNext()`).

The economy underneath is identical for every strategy: the tuned one from
economy.md (16 harvesters, refinery-on-queue at 5, carryall-on-trip at 1200), so
nothing here is measuring economy quality by accident.

## Running it

```bash
cd bin
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --war-matrix=200000,8
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --war-timing=200000,5
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --war-ladder=200000,5
```

| Flag | Arguments | What it does |
|---|---|---|
| `--war-matrix` | `ticks,maps` | every share against every other share |
| `--war-timing` | `ticks,maps` | schedules: flat shares against ones that change mid-match |
| `--war-ladder` | `ticks,maps` | repeatedly finds the best counter to the current champion |
| `--war-trace` | — | one line per house per 20000 ticks of every match |
| `--war-telemetry` | `ticks,step[,seed]` | plays one match and prints a sampled CSV of how it went |
| `--war` | `shareA,shareB[,seed]` | plays one pairing in the GUI, to watch |

Watching one: the match opens on the first base, and at t0 the second house owns
a single Construction Yard in the opposite corner of a 62x62 map -- which reads
as "there is only one AI here". **Tab** walks the camera from one base to the
next. The two overlay lines at the top of the viewport are the other check: if
both are there, both houses are.

`--war=0,45 --skirmish-self-test=20000` is the headless twin of the same match --
same shares, same seed, same map -- and prints both houses. It is the quickest
way to tell a display problem from a setup problem.

A match is 200000 ticks and takes about 0.35 s, so a seven-share round robin over
eight maps — 336 matches, both sides of every table — runs in a few minutes.

Every pairing is played twice per map with the sides swapped, because the two
houses cannot be the same House: the engine keeps one record per House id. Atreides
and Harkonnen have different units, different damage bonuses and opposite corners
of the map, and without the swap half of every result would be House identity.

## Reading a result

```
  mil 30% vs 60%: 10-2 points, value 120668-28063, actually spent 30%/56%
```

Two points for a win, one for a draw, out of `maps * 4`. A win means the enemy
lost its last structure, or — far more often — that the match ran out of ticks
with one side holding decisively more value than the other. Value is everything a
house still owns priced at what it cost, scaled by its remaining hitpoints, so a
base ground down to rubble scores as the rubble it is. Inside 20% it is a draw,
not a result.

**The `actually spent` column is the one that keeps the experiment honest.** A
share is a ceiling; whether a house gets anywhere near it depends on how fast its
factories run and how many unit slots it has. Without this column a share that
was never reached is indistinguishable from a share that did not matter — which
is exactly the mistake the first run of this matrix made, reporting a flat result
above 30% that turned out to be four plans all spending the same.

Requested against realised, measured:

| Requested | 0% | 15% | 30% | 45% | 60% | 75% | 90% |
|---|---:|---:|---:|---:|---:|---:|---:|
| Realised | 0% | 15% | 30% | 43% | 56% | 66% | 73% |

So the knob binds cleanly up to about 60% and then saturates: a house cannot put
more than roughly three quarters of its income into war, because at that point it
is limited by factory throughput and the 40 unit cap, not by money.

## What it found

### The level of the split barely matters

Seven shares, round robin, 200000 ticks, eight maps, both sides of every table:

| Share | 0% | 15% | 30% | 45% | 60% | 75% | 90% |
|---|---:|---:|---:|---:|---:|---:|---:|
| Points of 192 | 0 | 98 | 137 | 116 | 118 | 103 | 100 |

Spending nothing on the army loses every single match, which is the only thing
here that was never in doubt. Above that the whole range from 15% to 90% lands
within about 20% of each other, with 30% ahead — and 30% beats every other share
head to head, so this is a transitive ordering, not a cycle. **There is no
counter-strategy among constant shares.** Whatever a house is spending, the
answer is not to spend a different amount; it is to spend it at a different time.

### The timing of the split decides the match

Every schedule below spends nothing on war until its switch tick and then 90% of
income. Same round robin, 200000 ticks, six maps:

| Schedule | flat 30 | flat 45 | flat 90 | arm at 15k | arm at 25k | arm at 35k | arm at 50k | arm at 70k |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Points of 168 | 71 | 77 | 56 | 58 | **105** | **106** | **104** | 95 |

A flat share — any flat share — loses to a schedule that booms first, by a third.
The winning window is broad: anywhere between t25000 and t50000 scores the same,
and the default is the middle of it. Outside the window both directions are
punished, and for different reasons:

* **Arming earlier is not arming.** `flat 90` and `arm at 15k` score the same as
  each other and near the bottom, because before roughly t20000 there is no
  Barracks and no factory to spend on. A share is a share of *income*, and early
  income is tiny, so an early high share buys almost nothing while starving the
  economy that would have paid for a real army later. A genuine rush is not
  expressible as a share at all — it would need a different build order.
* **Arming later is losing.** `arm at 70k` still holds up; a switch at t110000 of
  a 200000 tick match scored 15 points of 168 in an earlier run — annihilated by
  everything.

### The switch point is absolute, not proportional

The same sweep at 400000 ticks, four maps, peaks in the same place: 25k → 73,
35k → 76, 50k → 71, with 15k (50) and 70k (52) clearly behind. Doubling the
match does not move the window, which says it is not a fraction of the match at
all — it is how long this economy takes to stand up. That is the number the
schedule is really keyed to.

So the rule the search settled on, and the default in
`Skirmish_MakeDefaultPlan()`: **nothing on the army until the base is built,
then most of the income.** 0% → 90% at t35000.

## Watching one match instead of comparing many

`--war-telemetry` is the only thing here that scores nothing. It plays a single
match and prints one row per house per step:

```
tick,house,refineries,combatStructures,harvesters,combatUnits,combatHitpoints,damageTaken,spiceRefined,powerSurplus
```

Six of those are levels. `damageTaken` and `spiceRefined` are running totals on
purpose: a rate is the difference between two samples, and differencing a total
is exact, while sampling a rate directly misses whatever happened in between.

Damage is recorded as **taken**, not dealt, because neither `Unit_Damage()` nor
`Structure_Damage()` is told who fired. In a two-house match "damage A dealt" is
"damage B took", with the caveat that Palace specials land in the same column --
which is fair enough, since they are the summoner's doing.

`./opendune --war=0,0,8919 --war-telemetry=200000,5000` is a contested one: level
at t150000 on harvesters and damage taken, and decided only after. What it shows,
and what the sweeps above cannot:

* the whole opening is economy and nothing else -- combat structures sit at zero
  for both until t30000, which is the default schedule doing its job;
* the army then appears almost vertically, because the share is a share of
  *accumulated* income and by t30000 there is a lot of it;
* damage arrives in waves rather than as pressure -- teams fill to their minimum
  size and leave together;
* **power surplus leads the collapse.** It goes negative before structures start
  falling: losing windtraps halves the hitpoints of everything else
  (`Structure_CalculateHitpointsMax`), and the finishing off gets faster from
  there. It is the earliest warning in the file.

## The Palace hands out an army the budget cannot see

Every House's Palace has a special weapon, and `Structure_Update()` fires it for
an AI the moment it comes off cooldown. Two of the three produce units:

| House | Special | Cadence | Belongs to |
|---|---|---:|---|
| Atreides | five Trooper/Troopers on ACTION_HUNT | 300 ticks | **HOUSE_FREMEN** |
| Ordos | a Saboteur | 300 ticks | itself |
| Harkonnen | a Death Hand missile | 600 ticks | itself |

The Atreides one is the problem. Those troopers are not Atreides units: they
belong to a House that has no base in the match, so **nothing charges them to any
military share**, and `House_AreAllied()` makes them permanent enemies of the
other AI and permanent friends of the one that summoned them. A strategy that
reaches the Palace gets a free, endlessly renewed army on top of its budget.

`Skirmish_GetBystanders()` counts them and `Skirmish_GetCasualties()` splits
losses by cause; `--skirmish-self-test` prints both at the end.

Finding them exposed a second bug, since fixed. `Unit_Create()` does not go
through `Unit_SetPosition()`, so the skirmish "everybody sees everybody" line did
not apply to units born straight onto the map -- and an unseen unit scores zero
in `Unit_GetTargetUnitPriority()`. The Fremen were therefore invisible to the AI
that was supposed to be shooting them. The casualty split says it plainly:

| Fremen losses at t400000 | shot | crushed |
|---|---:|---:|
| before | 9 | 111 |
| after | 63 | 43 |

Before, nine in ten died under a passing tank rather than to a weapon, and twenty
were alive at the end. After, they are killed about as fast as the Palace makes
them and none survive to the end of the match. The other two Houses were around
90% shot throughout, which is what made the Fremen figure stand out.

Two consequences, both real:

* **It breaks the budget model.** The whole premise here is that a share of
  income is the only thing separating two strategies. Free units are outside
  that.
* **It squeezes the unit pool.** The pool is 102 slots for the whole map, and
  the two houses are capped at 40 each. Twenty-one bystanders is twenty-one
  slots neither AI can build into, so the Palace also quietly throttles the
  loser's production.

The Palace is the last entry of the base plan but it is reached well inside the
200000 tick horizon -- both houses had it up by t120000 in a spot check -- so
this is present in the results above, not merely a risk for longer matches. The
side swap cancels the part of it that is House identity; it does not cancel the
free units.

The schedule sweep was re-run after the visibility fix and lands in the same
place -- 25k/35k/50k at 102/108/102 points of 168, with 15k and 70k at 79 and
every flat share between 59 and 72 -- so the conclusion above survives it. The
free units remain a known hole in the budget model rather than a demonstrated
distortion of this particular result.

## Caveats worth keeping in mind

* Matches rarely end in annihilation at this horizon. Most results are value
  margins, which is a weaker criterion than a kill — it says who was winning, not
  who won.
* Six to sixteen matches per pairing is a small sample, and the standings move
  between runs. Treat a two-point gap as noise.
* The horizon decides the answer here exactly as it did in the economy search.
  Any conclusion below should be read at two horizons before it is believed.
