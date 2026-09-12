# Harvester logic: state model, our changes, and the fix

## 0. Whose harvesters

Everything below was written for the player's harvesters, and
`Unit_Harvester_Update()` used to return immediately for any other house. It no
longer does while a skirmish is running: the AI's harvesters stall in exactly the
same ways, and in an AI vs AI match there is nobody to nudge them. The rest of
the module is house-agnostic — it scopes refineries and carryalls by
`Unit_GetHouseID(unit)`. See [skirmish.md](skirmish.md).

## 0. What "the legacy script" is here

OpenDUNE does not implement harvester behaviour in C. It runs the original
Dune II `UNIT.EMC` bytecode through `src/script/script.c`, and the C functions in
[`src/script/unit.c`](src/script/unit.c) are only the *opcodes* that bytecode
calls. `Unit_SetAction()` (unit.c) selects the branch:

```c
u->o.script.variables[0] = action;      /* ACTION_HARVEST, ACTION_MOVE, ... */
Script_Load(&u->o.script, u->o.type);
```

Everything below is read off the actual bytecode in `bin/data/DUNE.PAK`, with
[`tools/dis_emc.py`](tools/dis_emc.py):

```bash
python3 tools/dis_emc.py bin/data/DUNE.PAK UNIT.EMC --type 16     # harvester
python3 tools/dis_emc.py bin/data/DUNE.PAK UNIT.EMC --range 877 975
```

Word numbers below refer to that listing. `SCRIPT_JUMP_NE` jumps when the popped
value is *zero*, so the tool prints it as `JUMP_IF_ZERO`.

## 1. The legacy harvester cycle, as it is really written

### 1.1 Dispatcher (words 2123…)

```
var0 == ACTION_HARVEST (5)  → sub@748
var0 == ACTION_MOVE    (1)  → sub@637
var0 == ACTION_RETURN  (6)  → if (var4) ClearCarryallLink(var4)
                              GetInfo(6)          ; recompute originEncoded
                              SetDestination(0)   ; clear targetMove
                              sub@877(STRUCTURE_REFINERY)
var0 == STOP/GUARD          → Delay(60) loop
```

### 1.2 `sub@748` — ACTION_HARVEST

```
if (var4)  ClearCarryallLink(var4), var4 = 0      ; a harvest order drops any lift
if (amount >= 100) goto 870
loop:
    if (amount >= 100) goto 870
    if (targetMove != 0)  sub@23(targetMove)      ; drive there
    else if (Harvest())   Delay(5)                ; nibbling
    else {                                        ; not on spice
        SetDestination(SearchSpice(3)) … (20) … (40) … (60)
        if (targetMove == 0) {
            if (amount != 0) { SetDestination(0); SetAction(ACTION_RETURN) }
            else               SetAction(ACTION_STOP)
        }
    }
    Delay(10)
870: SetDestination(0); SetAction(ACTION_RETURN)  ; ← full: hand over to RETURN
```

Note what the harvest branch does *not* do: it never calls a carryall.

### 1.3 `sub@877` — the return trip (param = `STRUCTURE_REFINERY`)

```
loop:
  if (GetIndexType(var4) == IT_STRUCTURE) SetAction(ACTION_MOVE)   ; door reserved → drive
  if (var4 != 0) {                                  ; a lift or a door is booked
      if (FindStructure(REFINERY)) { Harvest(); Delay(30) }        ; WAIT, do nothing
      else                        { ClearCarryallLink(var4); var4 = 0 }
  } else if (FindStructure(REFINERY)) {
      if (GetInfo(7) == UNIT_HARVESTER) {
          if (UnitCount(CARRYALL) != 0) {
              var4 = CallUnitByType(CARRYALL);      ; ← the ONLY transport request
              if (var4) Harvest();                  ; booked → wait
              else      GoToClosestStructure(REFINERY);            ; walk
          } else        GoToClosestStructure(REFINERY);            ; walk
      } else var4 = CallUnitByType(CARRYALL);
      Delay(60)
  } else {
      Delay(60)                                     ; ← NO free refinery: PARK
  }
```

`Script_Unit_FindStructure()` accepts a refinery only when
`state == IDLE && o.linkedID == 0xFF && o.script.variables[4] == 0`.

**Three consequences drive every bug in this area:**

1. **`var4 != 0` on a harvester means the script deliberately stands still.**
2. **No refinery passing `FindStructure()` ⇒ every full harvester in the house
   parks in a `Delay(60)` loop.** One blocked refinery stops the entire economy.
3. **Transport is requested exactly once**, at word 932. A refusal in that single
   instant condemns the harvester to walk the whole way.

Point 2 is corroborated upstream by `Unit_DisplayStatusText()`:

```c
if (unit->actionID == ACTION_MOVE && Tools_Index_GetStructure(unit->targetMove) != NULL)
        stringID = STR_IS_D_PERCENT_FULL_AND_HEADING_BACK;
else if (unit->o.script.variables[4] != 0)
        stringID = STR_IS_D_PERCENT_FULL_AND_AWAITING_PICKUP;
```

## 2. The state model

### 2.1 Fields and who owns them

| Field | Owner | Meaning |
|---|---|---|
| `actionID` / `nextActionID` | `Unit_SetAction()` | Which EMC branch is loaded. `switchType 0` defers the switch into `nextActionID` while `currentDestination != 0`. |
| `targetMove` | script + our recovery code | The active order. **Structure index = returning to the refinery; tile = going to spice.** |
| `currentDestination`, `route[14]` | movement layer | Current step and the pathfinder result. `route[0] == 0xFF` = recompute. |
| `o.script.variables[4]` | `Object_Script_Variable4_{Link,Set,Clear}` | A **mutual** reservation. harvester↔carryall = "pickup booked, stand still". harvester↔refinery = "entrance reserved". refinery↔carryall = "transport inbound". |
| `originEncoded` | script | Long-term home refinery. `GetInfo(0x06)` *recomputes it for every harvester on demand* via `Unit_FindClosestRefinery()`. It is a preference, never an instruction. |
| `amount` | script | Cargo, 0…100. |
| `harvestCenter` | **ours** | Player-selected working area. |
| `harvestHoldPosition` | **ours** | Explicit player Move: stay put instead of resuming harvest. |
| `airTransitDestination` | **ours** | One-shot airlift request. |

### 2.2 Refinery-side state

`Unit_Harvester_RefineryAccepts()` must mirror exactly what
`Script_Unit_Pickup()` accepts:

```c
type == STRUCTURE_REFINERY && hitpoints != 0 &&
state == STRUCTURE_STATE_IDLE && o.linkedID == 0xFF && o.script.variables[4] == 0
```

Two traps hide in there:

* `Object_Script_Variable4_Set()` on a structure with `busyStateIsIncoming`
  **flips the structure state to BUSY** as a side effect.
* A harvester queued at the door legitimately holds `refinery.var4`. Judging the
  refinery "occupied" then means the harvester declares *its own* booking a
  blockage.

## 3. What we changed before this session, and what each change broke

| Commit | Intent | Defect it introduced |
|---|---|---|
| `99dba50c`, `94a32fde` | Refinery recovery, transport returns | `Unit_Harvester_RecoverRefinery()` + the `airTransitDestination` field. |
| `70928ae2`, `1e7cda3e` | Airlift harvesters out of the refinery | `Script_Structure_Unknown0C5A()` holds the unit **inside** the refinery while any carryall exists on the map. |
| `7fed11a5` | Stop the Harvest/Stop oscillation | (fine — the guard in `Script_Unit_SetAction()`.) |
| `7cbdaaa9` | Unify guard return and refinery recovery | Recovery now falls back to `originEncoded`, and books a carryall on a blocked door. |
| `44878b4b` | Keep collecting until full | `Unit_Harvester_ContinueUntilFull()` runs unthrottled and outranks an explicit Return order. |

### 3.1 Bug A — the sticky airlift (the reported symptom)

`UnitSelection_ResetOrder()` set, on every Harvest order:

```c
unit->airTransitDestination = packed;
```

Nothing cleared it except an actual carryall delivery. So a harvester that
walked to the field on its own kept the value for the rest of its life, and:

1. `Unit_AirTransit_Update()` runs every 20 ticks for **any** unit with a
   non-zero `airTransitDestination`, regardless of `amount`. It books a carryall
   and calls `Object_Script_Variable4_Link()`.
2. The now-full harvester sees `var4 != 0` → **parks**.
3. `Script_Unit_Pickup()` saw the same stale field and flew it to the *spice
   tile*, not the refinery.

Net effect: full harvester stands still, then gets airlifted back onto the field
it just emptied. Exactly "загрузился и не ехал на разгрузку".

Bytecode confirmation: with `airTransitDestination` set, `Unit_AirTransit_Update()`
links `var4`, so `sub@877` takes the `if (var4 != 0)` branch at word 892 and sits
in `Harvest(); Delay(30)` — the harvester waits, and the carryall then delivers it
to the spice tile.

### 3.2 Bug B — the refinery held its own door shut

`Script_Structure_Unknown0C5A()`:

```c
carryall = Unit_CallUnitByType(UNIT_CARRYALL, ...);
if (carryall == NULL) {
        if (Unit_IsTypeOnMap(s->o.houseID, UNIT_CARRYALL)) return 0;   /* keep waiting */
} else {
        Object_Script_Variable4_Set(&s->o, ...);                       /* one-sided! */
        return 0;
}
```

* `return 0` keeps the finished harvester **inside** the refinery, so
  `o.linkedID != 0xFF`. That refinery now fails `Script_Unit_FindStructure()`,
  and by consequence #2 above **every** full harvester in the house falls into
  the `Delay(60)` park loop at word 959. One busy carryall froze the whole spice
  economy. This is the most likely path to the reported symptom.
* `Object_Script_Variable4_Set()` sets only the structure side. If the carryall
  died in transit, `refinery.var4` stayed non-zero forever and the refinery was
  permanently invisible to `Script_Unit_Pickup()` and `Unit_FindClosestRefinery()`.

### 3.3 Bug C — the carryall leak in the stall recovery

`Unit_Harvester_RecoverRefinery()` booked a carryall whenever the entrance was
blocked. But `Script_Unit_Pickup()` ends with

```c
if (s == NULL) return 0;      /* no free refinery → give up, keep the booking */
```

so the transport stayed bound to the harvester (`targetMove` + `var4`) forever,
and `Unit_CallUnitByType()` skips any carryall with `targetMove != 0`. Every
blocked entrance permanently consumed one carryall, and each consumed carryall
parked its harvester.

### 3.4 Bug D — `originEncoded` treated as an order

```c
refinery = Tools_Index_GetStructure(unit->targetMove);
if (refinery == NULL && Tools_Index_GetType(unit->originEncoded) == IT_STRUCTURE)
        refinery = Tools_Index_GetStructure(unit->originEncoded);
```

`GetInfo(0x06)` refreshes `originEncoded` to a refinery for *every* harvester, so
this matched a harvester quietly sitting on spice. Standing on spice is not
"movement", so after 180 ticks the recovery declared it stalled and cancelled its
harvest — every three seconds. (This one was already reverted in the working tree
before this session; it is kept out.)

### 3.5 Bug E — self-inflicted "refinery occupied"

`RefineryAccepts()` rejected a refinery whose `var4` pointed at the *asking*
harvester, i.e. its own door reservation, which then fed straight into bug C.

**That fix was written but never reached.** See §6: the `state != IDLE` test
above it answered first, and a booked door is a `BUSY` door, so the `var4` line
was unreachable in the normal case and bug E went on happening under a different
name for the whole of §5.

### 3.6 Smaller defects

* `s_harvester*[UNIT_INDEX_MAX]` were never reset when a pool index was reused.
* `Unit_Harvester_ContinueUntilFull()` ran on every 20-tick pass and calls
  `Unit_Harvester_FindPreferredSpice()` — a 64×64 sweep plus up to 24 pathfinder
  runs — per player harvester. The same function is also called from script
  context by the carryall pickup and the refinery release.
* `ContinueUntilFull()` cancelled an explicit player **Return** order.
* Two copies of "apply a targeted order": `UnitSelection_ResetOrder()` in unit.c
  and an inline block in `GUI_Widget_Viewport_Click()`. Only the first booked the
  airlift, so a single harvester and a selected group obeyed different Harvest
  orders.

## 4. The fix

### 4.1 Principle

The legacy script owns the harvester. Our code is a set of **recovery paths** and
may only act when the script has left the unit with nothing useful to do. It may
never create a reservation that the other side cannot honour.

### 4.2 Changes

**`src/unit.c`**

* All per-unit bookkeeping moved into one documented `HarvesterTracker` struct,
  reset from `Unit_Create()` so a reused pool index starts clean.
* A block comment above the section states the cycle and the field ownership
  table from §2.
* `Unit_Harvester_FindPreferredSpice()` memoises its result per unit for 30 game
  ticks, invalidated by a change of `harvestCenter`.
* `Unit_Harvester_RefineryAccepts(refinery, unit)` accepts a door reservation
  held by the asking unit (bug E).
* New `Unit_Harvester_BeginOrder(unit, action)` — the single place a player order
  resets harvester intent: clears the airlift, sets `harvestHoldPosition`,
  records an explicit Return, resets the stall timers. Called from
  `UnitSelection_ResetOrder()`, `UnitSelection_BeginAction()`, the air-transit
  path and the classic one-unit action panel.
* New `Unit_Harvester_UpdateAirlift()` — the airlift is now **one-shot**. It is
  consumed on arrival (within 2 tiles), on filling up, on a Move order, and after
  a 600-tick wait for transport that never came; then the harvester takes the
  ground route (bug A).
* `Unit_Harvester_RecoverRefinery()` no longer books transport. It re-routes to a
  refinery that genuinely accepts, otherwise it recomputes the route and stays in
  the queue, keeping its door reservation (bug C).
* New `Unit_Harvester_RecoverFullCargo()` — watchdog: a full harvester with no
  destination and no pickup reservation is sent to a refinery as `ACTION_MOVE`,
  which is exactly what the legacy script does when no carryall is free. This is
  the backstop that makes the reported symptom unreachable regardless of cause.
* New `Unit_Harvester_RetryLift()` — see §4.3.
* `Unit_Harvester_ContinueUntilFull()` respects an explicit Return and an
  explicit Move, and reuses the shared helpers.
* `Unit_AirTransit_Update()` never lifts a loaded harvester.
* New public `UnitSelection_IssueOrder()`; `GUI_Widget_Viewport_Click()` now calls
  it instead of its own copy of the order sequence.

**`src/script/unit.c`**

* `Script_Unit_Pickup()` ignores `airTransitDestination` for a loaded harvester.
* `Script_Unit_Pickup()` releases the carryall (`var4` + `targetMove`) when no
  structure can receive the unit, instead of sitting on the reservation (bug C).

**`src/script/structure.c`**

* `Script_Structure_Unknown0C5A()` holds the unit back **only** when a transport
  was actually booked, and links both sides with `Object_Script_Variable4_Link()`
  (bug B).

### 4.3 Retrying the airlift while walking

Consequence #3 of §1.3: `sub@877` asks for a carryall exactly once, at word 932.
If every transport happened to be busy in that instant the script fell through to
`GoToClosestStructure()` and the harvester walked the whole map, even when a
carryall became free one second later. The outbound trip to the spice field never
asks at all.

`Unit_Harvester_RetryLift()` closes that gap. Every 60 game ticks, for a
travelling harvester with no reservation of its own:

* the remaining distance to `targetMove` must exceed
  `HARVESTER_AIRLIFT_MIN_DISTANCE` (7 tiles) — below that a flight costs more in
  approach and unload time than it saves;
* a carryall must be genuinely free (`Unit_Harvester_HasFreeCarryall()`, the same
  predicate `Unit_CallUnitByType()` uses).

Then:

* **Outbound** (`amount < 100`, tile target): set `airTransitDestination`;
  `Unit_AirTransit_Update()` books the transport on the same pass.
* **Homebound** (`amount == 100`): a refinery that `Script_Unit_Pickup()` will
  accept must exist. Our own door reservation would hide exactly the refinery we
  are driving to, so it is released first — which is precisely what the original
  script does at word 906 when it gives up on transport. Then the carryall is
  booked directly; `Script_Unit_Pickup()` chooses the refinery itself.

The harvester **keeps driving** while the booking is outstanding: the transport
intercepts it en route, so a request that comes to nothing costs no time. Only
`Unit_CallUnitByType()`'s own bookkeeping is touched, never the harvester's `var4`
— so `sub@877` stays in its `GoToClosestStructure()` branch and never parks.

### 4.4 Resulting invariants

1. A harvester's `var4` is non-zero only while a carryall exists that can finish
   the trip; otherwise the reservation is released by the pickup opcode.
2. `airTransitDestination` is consumed exactly once.
3. A refinery is never left reserved by a dead carryall, and never left occupied
   by a unit waiting for one.
4. A full harvester either moves toward a refinery, or is inside one, or is
   airborne. Any other combination is repaired within 90 game ticks.
5. `originEncoded` is only ever read, never written by our code.

## 5. Choosing a different refinery, and the livelock that cost

The original script picks a refinery once and keeps driving to it whatever
happens there. With a single refinery that is fine. With several it produces the
one thing a fleet must not do: a queue at the first door and idle doors behind
it. `Unit_Harvester_RecoverRefinery()` therefore re-aims a return trip as soon as
its refinery refuses it, rather than waiting out the 180 tick stall timer.

Rate limiting that is not a detail, it is the whole of it.

The script re-aims the unit back at *its* choice on its own tick. A switch on
every one of ours is then a tug of war, and the harvester loses: each switch calls
`Unit_Harvester_ClearOrder()` and recomputes the route, so the unit never
completes a single step. It stands in the doorway for the rest of the match with
an empty refinery beside it — the fleet looks alive, `harvesters` counts them all,
and refined spice simply stops.

`HARVESTER_RETARGET_COOLDOWN` (60 ticks) is the fix: still an order of magnitude
faster than the stall timer this replaced, and slow enough that a step can finish.

**How it showed up.** Not as a crash and not as a stall anyone could see — as
noise in a search. The economy sweep had two reproducible collapses (N=10 in both
rows, N=20 in one) that survived every explanation offered for them: power,
routing, counting. They were this. Which threshold happened to produce the
standoff was luck, which is exactly why it looked patternless. With the cooldown
the sweep is flat within 4% and both collapses are gone — see
[economy.md](economy.md).

### What was still broken, and what it turned out to be

The cooldown treats the symptom. §6 is the cause.

```
ref#3 state1 linked255 var4=4016
```

State 1 is `STRUCTURE_STATE_BUSY`, `linkedID` 255 means nothing is inside, and
`var4` is a door reservation held by a harvester that is standing outside. The
refinery went busy expecting a harvester that never entered, and it never comes
back out of that state — so it refuses every harvester in the house, forever.

It reproduces deterministically and cheaply:

```bash
cd bin
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./opendune --economy-baseline=80000,1 --economy-trace
```

Baseline 1 (three refineries, one harvester, no Heavy Factory) freezes at around
t30000 and refines nothing for the remaining 50000 ticks. `--economy-trace` prints
the state of every harvester and every refinery at each sample, which is what
found it.

## 6. The tug of war was with itself

§5 blames the script for re-aiming the unit back at its choice. It does not, and
measuring that is what found the real bug: in the steady state the script sits in
the `ACTION_MOVE` branch and only recomputes a route. **The C layer was flipping
on its own, unassisted.**

Aiming at a refinery books it, and booking it marks it busy:

```
Unit_SetDestination(u, refinery)
  -> Object_Script_Variable4_Link(unit, refinery)     unit.c:4162
     -> Object_Script_Variable4_Set(refinery, unit)   object.c:63
        -> Structure_SetState(s, STRUCTURE_STATE_BUSY)   /* busyStateIsIncoming */
```

`Unit_Harvester_RefineryAccepts()` then asked `state != STRUCTURE_STATE_IDLE`
**before** it asked whose booking had put it there, so every homebound harvester
read its own destination as one that refused it. With one refinery that is
harmless — `FindAvailableRefinery(unit, refinery)` excludes the current target
and returns `NULL`, so nothing happens, which is why the bug hid for so long.
With two it is a loop:

1. `RefineryAccepts(A)` is false, because *we* made A busy.
2. `HARVESTER_RETARGET_COOLDOWN` expires; the alternate is B, which is idle.
3. `Unit_Harvester_ClearOrder()` releases A — A goes back to `IDLE` —
   and `Unit_SetDestination(B)` books B, so B goes `BUSY`.
4. Sixty ticks later, with A and B swapped. For ever.

Each flip calls `Unit_Harvester_StopRoute()`, so the harvester tears up its route
about as often as it recomputes one and gets nowhere. `refineryStalledSince` is
rewritten on every flip, so the 180-tick stall path below it never runs either.

**The fix is to read the booking before the state.** `BUSY` with `linkedID ==
0xFF` and `var4 == me` is a door this harvester is expected at, and it accepts.
`READY` still refuses (somebody is unloading inside) and somebody else's booking
still refuses. The three script-side pickers — `Script_Unit_FindStructure`,
`Script_Unit_GoToClosestStructure`, `Script_Unit_Pickup` — are deliberately left
alone: for *them* a booked refinery genuinely is taken.

Two things fall out of it, both of which were the same bug wearing a different
hat. `Unit_Harvester_IsQueued()` is `!RefineryAccepts(...)`, so it used to answer
"queueing" for every harvester merely driving home — contradicting its own doc
comment, showing `WAIT REF` in the trace where `TO REF` belonged, and feeding the
AI's "build another refinery" rule at [skirmish.c](src/skirmish.c). And
`Unit_FindClosestRefinery()`'s "a choice already made is kept" guard was dead for
the same reason; it works now.

Measured on `--economy-baseline=80000,3`, before against after:

| baseline | refineries | before | after |
|---|---|---|---|
| 0 | one | 11963 | 12134 |
| 1 | three | 4730 | 5495 |
| 2 | two | 1723 | 5228 |

The one-refinery case barely moves, which is the diagnosis confirming itself: it
is the only shape that cannot ping-pong. On `--war-metrics`, `econ.spice/match`
73597 → 83309 and `result.points %` 87 → 91, PASS with no regressions.

`--harvester-self-test` is the guard, and it plays rather than asserts: two
refineries as far apart as the map allows, one loaded harvester aimed at one of
them, and thirty seconds of the real game loop. What is counted is how often the
destination changes — a loop is what is being ruled out, so the gate is on the
count and not on the final answer. Before the fix it counts exactly 30, one per
sixty-tick cooldown.

### What is still broken

The refinery state that §5 called stuck is not stuck any more — `state1
linked255 var4=<harvester>` now resolves to `state2 linked<harvester>` as the
harvester arrives, rather than standing for the rest of the match. But the leak
underneath it is real and untouched: `sub@877` words 906-910 do

```
906: PUSHVAR 4 / CALL 36 ClearCarryallLink / STACKREWIND 1 / PUSH2 0 / POPVAR 4
```

and `Script_Unit_Unknown2552()` ([script/unit.c](src/script/unit.c)) returns
without doing anything unless `variables[4]` is a **carryall**. When it is a
refinery — a door booking — the `POPVAR 4` blanks the harvester's side directly,
bypassing `Object_Script_Variable4_Clear()`, and the refinery keeps pointing at a
harvester that has forgotten about it. With the fix above that no longer refuses
the whole house, because only *other* harvesters are turned away; but if the
holder dies still holding it, the door stays booked to nobody.
