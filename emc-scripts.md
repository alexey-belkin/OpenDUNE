# The EMC script layer

Everything a unit, structure or team *decides* lives in Westwood's original
bytecode, not in this repository. OpenDUNE supplies the virtual machine and the
opcodes; the behaviour is data the player must own. Read this before changing
anything that affects what an object does on its own.

## Where the logic is

| Script | Size | Drives |
|---|---:|---|
| `UNIT.EMC` | 5482 B | every unit type, one entry per type |
| `BUILD.EMC` | 1070 B | structures |
| `TEAM.EMC` | 346 B | AI teams |

All three sit inside `DUNE.PAK`. Without them units simply do not move — there
is no C fallback.

## How a script is entered

`Unit_SetAction()` does not call a C function. It writes the action into
variable 0 and loads the bytecode for the object type:

```c
u->o.script.variables[0] = action;      /* ACTION_HARVEST, ACTION_MOVE, ... */
Script_Load(&u->o.script, u->o.type);   /* → offsets[type] in UNIT.EMC */
```

The script then dispatches on variable 0. `GameLoop_Unit()` runs up to
`SCRIPT_UNIT_OPCODES_PER_TICK` opcodes per object every 5 game ticks (3 when the
object is off-screen).

* VM: `Script_Run()` in [src/script/script.c](src/script/script.c) — 19 opcodes.
* Callable functions: `g_scriptFunctions{Unit,Structure,Team}` in the same file,
  implemented in [src/script/unit.c](src/script/unit.c),
  [structure.c](src/script/structure.c), [team.c](src/script/team.c).

## Reading the bytecode

```bash
python3 tools/dis_emc.py bin/data/DUNE.PAK --list
python3 tools/dis_emc.py bin/data/DUNE.PAK UNIT.EMC              # entry offsets
python3 tools/dis_emc.py bin/data/DUNE.PAK UNIT.EMC --type 16    # harvester
python3 tools/dis_emc.py bin/data/DUNE.PAK UNIT.EMC --range 877 975
```

Addresses are 16-bit word indices into the `DATA` chunk, and jump targets use
the same numbering, so the listing is directly navigable.

**Gotcha:** `SCRIPT_JUMP_NE` jumps when the popped value is **zero**. The
disassembler prints it as `JUMP_IF_ZERO`; reading it as "jump if not equal"
inverts every branch in the file.

## Shared state, and who owns it

Scripts and C code write the same fields. Almost every behaviour bug in this
repository has been a violation of one of these:

| Field | Rule |
|---|---|
| `o.script.variables[0]` | The current action. Owned by `Unit_SetAction()`. |
| `o.script.variables[4]` | A **mutual** reservation between two objects. Only ever touch it through `Object_Script_Variable4_{Link,Set,Clear}`. unit↔carryall means "transport booked, stand still"; unit↔structure means "entrance reserved". Setting it makes the object *wait*, so only set it when the other side can actually finish the job. `Object_Script_Variable4_Set()` on a structure with `busyStateIsIncoming` also flips its state to BUSY. |
| `targetMove` | The active order. A structure index and a tile index mean different things to the script; check `Tools_Index_GetType()`. |
| `originEncoded` | A long-term preference, never an instruction. `Script_Unit_GetInfo(0x06)` recomputes it on demand — for harvesters it is *always* a refinery, so it can never be used to detect "is returning home". |
| `currentDestination` | The tile the current step is aimed at, owned by the movement layer. **Never clear it while `unit->speed != 0`:** `Unit_Move()` then measures progress against tile (0,0), decides the unit arrived and stops it at a half-tile offset. Clear `route[0]` instead and let the step finish. |
| `route[14]` | The computed path. `route[0] = 0xFF` asks for a re-path; safe at any time. |

## Supplementing a script from C

The script owns the object. C code added on top is a set of *recovery paths*
and may only act when the script has left the object with nothing useful to do.
Two consequences worth internalising:

1. Scripts contain deliberate **wait states**. A `Delay(60)` loop is not a bug in
   our code; it is what the script does when its preconditions fail. Fix the
   precondition, do not fight the script.
2. Before changing behaviour, disassemble the relevant branch. Guessing what the
   script does from the opcode list is how the wrong fix gets written.

[harvester.md](harvester.md) is a worked example: the full harvester state model,
the bytecode listing, five defects that came from breaking the rules above, and
what the corrected design looks like.
