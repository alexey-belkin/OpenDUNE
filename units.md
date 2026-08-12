# OpenDUNE / Dune II units

> Prefer a polished, searchable layout? Open the [HTML version](units.html).

This is the complete unit-type table used by OpenDUNE. It includes normal
player/campaign units as well as internal projectile, weapon-effect, transport,
and special-object types defined by the engine.

## Reading the table

- **Cost** and **Build time** are the values in the engine's `ObjectInfo` table.
  Build time is an internal game-time value, not seconds.
- **Range** is the engine's `fireDistance` value. It is omitted for units with
  no weapon.
- **Damage** is the engine's `damage` value for the listed unit or projectile.
  A unit may launch a separate projectile with its own damage value.
- **Houses** lists the houses encoded as available for that unit type. `All`
  means all six houses: Harkonnen, Atreides, Ordos, Fremen, Sardaukar, and
  Mercenary.
- `—` means not applicable. Internal weapon/effect types are not buildable
  units even when their house mask is `All`.

## Complete unit list

| ID | Unit | Category / role | Movement | HP | Cost | Build time | Weapon / projectile | Damage | Range | Houses / access | Key characteristics |
|---:|---|---|---|---:|---:|---:|---|---:|---:|---|---|
| 0 | Carryall | Transport | Winged | 100 | 800 | 64 | None | — | — | All; support transport | Automatically carries harvesters and other pickup-capable units; cannot be directly selected. |
| 1 | Ornithopter (`'Thopter`) | Air attack unit | Winged | 25 | 600 | 96 | Missile Trooper | 50 | 50 | Atreides, Ordos, Fremen, Sardaukar, Mercenary; House of Ix, upgrade 1 | Fires twice; explodes on death; can target air units. |
| 2 | Infantry | Light infantry squad | Foot | 50 | 100 | 32 | Bullet | 3 | 2 | Atreides, Ordos, Fremen, Sardaukar, Mercenary; upgrade 1 | Fires twice; basic foot unit. |
| 3 | Troopers | Heavy infantry squad | Foot | 110 | 200 | 56 | Bullet | 5 | 5 | Harkonnen, Ordos, Fremen, Sardaukar, Mercenary; upgrade 1 | Fires twice; can target air units; tougher and longer-ranged than Infantry. |
| 4 | Soldier | Light infantry soldier | Foot | 20 | 60 | 32 | Bullet | 3 | 2 | All | Does not have the squad's two-shot flag; can wobble on suitable terrain. |
| 5 | Trooper | Heavy infantry soldier | Foot | 45 | 100 | 56 | Bullet | 5 | 5 | Harkonnen, Ordos, Sardaukar, Mercenary | Can target air units; can wobble on suitable terrain. |
| 6 | Saboteur | Special infiltrator | Foot | 10 | 120 | 48 | Bullet / sabotage | 2 | 2 | Ordos; special unit | Fast; has a Sabotage command; intended to destroy structures. |
| 7 | Launcher | Long-range artillery | Tracked | 100 | 450 | 72 | Rocket | 75 | 9 | Harkonnen, Atreides, Fremen, Sardaukar, Mercenary; upgrade 2 | Fires twice; explodes on death; can target air units; leaves tracked marks. |
| 8 | Deviator | Gas artillery | Tracked | 120 | 750 | 80 | GRocket / deviator gas | — | 7 | Ordos; House of Ix | Gas projectile temporarily deviates enemy units; explodes on death. |
| 9 | Tank | Main battle tank | Tracked | 200 | 300 | 64 | Bullet | 25 | 4 | All | Turreted; explodes on death; leaves tracked marks. |
| 10 | Siege Tank | Heavy tank | Tracked | 300 | 600 | 96 | Bullet | 30 | 5 | All; upgrade 3 | Turreted; fires twice; explodes on death; leaves tracked marks. |
| 11 | Devastator | Super-heavy tank | Tracked | 400 | 800 | 104 | Bullet | 40 | 5 | Harkonnen, Fremen, Sardaukar, Mercenary; House of Ix | Turreted; fires twice; explodes on death; has a self-destruct command. |
| 12 | Sonic Tank | Sonic weapon tank | Tracked | 110 | 600 | 104 | Sonic Blast | 60 | 8 | Atreides, Fremen, Sardaukar, Mercenary; House of Ix | Sonic weapon; protected from sonic-blast damage; explodes on death. |
| 13 | Trike | Light attack vehicle | Wheeled | 100 | 150 | 40 | Bullet | 5 | 3 | Atreides, Fremen, Sardaukar, Mercenary | Fires twice; fast and maneuverable; explodes on death. |
| 14 | Raider Trike | Fast attack vehicle | Wheeled | 80 | 150 | 40 | Bullet | 5 | 3 | Ordos, Fremen, Sardaukar, Mercenary | Faster than the Trike; fires twice; explodes on death. |
| 15 | Quad | Heavy attack vehicle | Wheeled | 130 | 200 | 48 | Bullet | 7 | 3 | All; upgrade 1 | Fires twice; explodes on death. |
| 16 | Harvester | Spice harvester | Harvester | 150 | 300 | 64 | None | — | — | All; refinery support | Collects spice and returns it to a Refinery; can be carried by a Carryall; explodes on death. |
| 17 | MCV | Mobile construction vehicle | Tracked | 150 | 900 | 80 | None | — | — | All; upgrade 1 | Deploys into a Construction Yard; explodes on death. |
| 18 | Death Hand | House superweapon projectile | Winged | 70 | — | — | Death Hand warhead | 100 | 15 | Harkonnen; internal weapon | House missile object; not a normal selectable or buildable unit. |
| 19 | Rocket | Launcher projectile | Winged | 70 | — | — | Explosive rocket | 75 | 8 | Internal weapon | Inaccurate; creates an impact on sand; animated projectile. |
| 20 | ARocket | Turret/advanced rocket projectile | Winged | 70 | — | — | Explosive rocket | 75 | 60 | Internal weapon | Accurate, long-range rocket; creates an impact on sand; animated projectile. |
| 21 | GRocket | Deviator projectile | Winged | 70 | — | — | Deviator gas | 75 | 7 | Internal weapon | Inaccurate gas projectile; creates the Deviator effect. |
| 22 | MiniRocket | Missile Trooper projectile | Winged | 70 | — | — | Mini-rocket | 0 | 3 | Internal weapon | Animated short-range projectile; damage is applied through its explosion/effect handling. |
| 23 | Bullet | Standard projectile | Winged | 1 | — | — | Bullet | 0 | — | Internal weapon | Short-lived projectile; explodes on impact. |
| 24 | Sonic Blast | Sonic Tank effect | Winged | 1 | — | — | Sonic wave | 25 | 10 | Internal weapon/effect | Area effect; ignores units with sonic protection. |
| 25 | Sandworm | Native creature / hazard | Slither | 1000 | — | — | Swallow / bite | 300 | — | Fremen scenario unit | Can swallow units on sand; starts with three units to eat; cannot be deviated. |
| 26 | Frigate | Transport / Starport object | Winged | 100 | — | — | None | — | — | Starport system object | Represents the ship that delivers Starport orders; not a normal combat unit. |

## Source

The values above are taken from the unit enumeration in
[`src/unit.h`](src/unit.h) and the corresponding `UnitInfo` records in
[`src/table/unitinfo.c`](src/table/unitinfo.c). Movement names are defined in
[`src/table/movementtype.c`](src/table/movementtype.c), and house/structure
flags are defined in [`src/house.h`](src/house.h) and
[`src/structure.h`](src/structure.h).
