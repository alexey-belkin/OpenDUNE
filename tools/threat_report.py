#!/usr/bin/env python3
"""Unit threat model for OpenDUNE.

Reads the engine's own tables -- it does not carry a copy of them -- and derives
a threat number per unit, then a class-versus-class matrix from the combat-class
damage multipliers.

    threat = DPS * hitpoints * firesTwiceCorrection

Sources parsed:
    src/table/unitinfo.c   hitpoints, damage, fireDelay, firesTwice, buildCredits
    src/unit.c             s_combatBalance damage matrix, Unit_CombatBalance_GetClass()

Nothing here is a second copy of a balance number: change the table or the
matrix in the source and this script follows.  The only judgement calls live in
EFFECTIVE_DAMAGE below, and each carries the code site it comes from.

Usage:
    python3 tools/threat_report.py                 # every table
    python3 tools/threat_report.py --json          # machine-readable
    python3 tools/threat_report.py --trooper-range # rocket infantry at >2 tiles
    python3 tools/threat_report.py --flat          # multipliers off, base stats only
    python3 tools/threat_report.py --duel          # explicit 1000-credit squad duels
"""

import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# --- timing ---------------------------------------------------------------
#
# g_timerGame runs at 60 Hz (opendune.c: Timer_Add(Timer_Tick, 1000000 / 60)).
# The unit loop's movement pass runs every 3 of those ticks (unit.c:2841), and
# that pass is where u->fireDelay is decremented (unit.c:3035).  So the fire
# cooldown counter ticks at 20 Hz.
TICKS_PER_SECOND = 60.0 / 3.0

# After a shot: u->fireDelay = ui->fireDelay * 2, then += Tools_Random_256() & 1
# (script/unit.c:730-739).  The random term is 0 or 1 with equal probability.
COOLDOWN_MULTIPLIER = 2
COOLDOWN_RANDOM_MEAN = 0.5

# firesTwice units put a 5-tick gap between the two shots of a pair
# (script/unit.c:734), so a pair costs 2*fireDelay + 5 plus two random terms.
FIRE_TWICE_GAP = 5

# firesTwice is gated on hitpoints > max/2 (script/unit.c:673), so a unit fires
# at full rate for the top half of its health bar and at half rate for the
# bottom half.  Averaged over the whole bar that is (1 + 0.5) / 2 = 0.75 of the
# full-health damage output.
FIRE_TWICE_LIFETIME_CORRECTION = 0.75


# --- damage the engine actually delivers ----------------------------------
#
# The `damage` column is not always what lands on a single target.  Each entry
# below names the code site that changes it.
def effective_damage(name, table_damage, trooper_at_range):
    if name == "'Thopter":
        # bulletType is UNIT_MISSILE_TROOPER, and that case subtracts a quarter
        # before the bullet is created (script/unit.c:708).
        return table_damage - table_damage // 4, "MISSILE_TROOPER: -25%"
    if name == "Sonic Tank":
        # Not a shot but a beam.  It walks tile by tile dealing hitpoints/4 + 1
        # and losing one hitpoint per step (unit.c:4504), so one target standing
        # in its path takes roughly a quarter of the nominal number -- while a
        # line of targets each take their own hit.
        return table_damage // 4 + 1, "beam: hp/4+1 per tile"
    if trooper_at_range and name in ("Trooper", "Troopers"):
        # Beyond 512 (two tiles) the bullet becomes UNIT_MISSILE_TROOPER and
        # loses the same quarter (script/unit.c:675).  Their range is five
        # tiles, so this is the ordinary case, not the exception.
        return table_damage - table_damage // 4, "MISSILE_TROOPER at >2 tiles"
    return table_damage, ""


# --- parsers --------------------------------------------------------------

def _field(block, key):
    m = re.search(r'/\* ' + key + r'\s+\*/ ([^,\n]+)', block)
    return m.group(1).strip().strip('"') if m else None


def parse_unitinfo(path):
    """hitpoints, damage, fireDelay, firesTwice, buildCredits per unit name."""
    text = open(path).read()
    units = {}
    for index, block in re.findall(r'\{ /\* (\d+) \*/(.*?)\n\t\},', text, re.S):
        name = _field(block, 'name')
        units[name] = {
            'index': int(index),
            'name': name,
            'hitpoints': int(_field(block, 'hitpoints')),
            'damage': int(_field(block, 'damage')),
            'fireDelay': int(_field(block, 'fireDelay')),
            'firesTwice': _field(block, 'firesTwice') == 'true',
            'isNormalUnit': _field(block, 'isNormalUnit') == 'true',
            'sonicProtection': _field(block, 'sonicProtection') == 'true',
            'bulletType': _field(block, 'bulletType'),
            'cost': int(_field(block, 'buildCredits')),
            'range': int(_field(block, 'fireDistance')),
        }
    return units


def parse_class_matrix(path):
    """The s_combatBalance damage matrix, as the compiled-in defaults."""
    text = open(path).read()
    body = re.search(r's_combatBalance = \{(.*?)\n\};', text, re.S).group(1)
    # Only the matrix rows are brace groups of bare integers; the scalars that
    # follow it in the initialiser are not braced.
    rows = re.findall(r'\{\s*((?:\d+\s*,\s*)+\d+)\s*\}', body)
    matrix = [[int(v) for v in row.split(',')] for row in rows]
    size = len(matrix)
    if not matrix or any(len(r) != size for r in matrix):
        raise SystemExit('could not read a square damage matrix from %s' % path)
    return matrix


def parse_unit_enum(path):
    """UNIT_* -> table index, from the UnitType enum in src/unit.h."""
    text = open(path).read()
    body = re.search(r'typedef enum UnitType \{(.*?)\n\} UnitType;', text, re.S).group(1)
    out = {}
    for name, value in re.findall(r'(UNIT_\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)', body):
        out[name] = int(value, 0)
    return out


def parse_unit_tuning(path, enum):
    """The s_unitTuning table: unit index -> (damage percent, rate percent).

    These are per-unit balance decisions that do not follow the class lines --
    the Siege Tank and Devastator without the Tank, the Raider Trike alone -- so
    they live in their own table in unit.c rather than in the class matrix.
    """
    text = open(path).read()
    body = re.search(r's_unitTuning\[\] = \{(.*?)\n\};', text, re.S).group(1)
    out = {}
    for name, dmg, rate in re.findall(r'\{\s*(UNIT_\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', body):
        out[enum[name]] = (int(dmg), int(rate))
    return out


def tuned_damage(damage, percent):
    """Unit_CombatBalance_ScaleDamage(), to the same rounding."""
    if damage == 0 or percent == 0:
        return 0
    return min((damage * percent + 50) // 100, 0xFFFF)


def tuned_fire_delay(delay, fires_twice, percent):
    """Unit_CombatBalance_SetRate(), to the same rounding.

    The rate is the inverse of the whole firing cycle, and for a firesTwice unit
    that cycle carries the short gap inside the doublet -- scaling fireDelay on
    its own would miss the target by exactly that gap.
    """
    if percent in (0, 100) or delay == 0:
        return delay
    gap = 6 if fires_twice else 1
    cycle = delay * 2 + gap
    cycle = (cycle * 100 + percent // 2) // percent
    if cycle <= gap:
        return 1
    return max((cycle - gap + 1) // 2, 1)


def parse_class_members(path):
    """UNIT_* -> class name, read out of Unit_CombatBalance_GetClass()."""
    text = open(path).read()
    body = re.search(r'Unit_CombatBalance_GetClass\(UnitType type\)\s*\{(.*?)\n\}',
                     text, re.S).group(1)
    members = {}
    pending = []
    for line in body.splitlines():
        for unit in re.findall(r'case (UNIT_\w+):', line):
            pending.append(unit)
        got = re.search(r'return COMBAT_CLASS_(\w+);', line)
        if got and pending:
            cls = got.group(1)
            if cls != 'NONE':
                for unit in pending:
                    members[unit] = cls
            pending = []
    return members


def parse_class_order(path):
    """Declaration order of the CombatClass enum, minus MAX/NONE."""
    text = open(path).read()
    body = re.search(r'typedef enum CombatClass \{(.*?)\n\} CombatClass;', text, re.S).group(1)
    names = re.findall(r'COMBAT_CLASS_(\w+)', body)
    return [n for n in names if n not in ('MAX', 'NONE')]


UNIT_ENUM_TO_NAME = {
    'UNIT_SOLDIER': 'Soldier', 'UNIT_INFANTRY': 'Infantry',
    'UNIT_TROOPER': 'Trooper', 'UNIT_TROOPERS': 'Troopers',
    'UNIT_TRIKE': 'Trike', 'UNIT_RAIDER_TRIKE': 'Raider Trike', 'UNIT_QUAD': 'Quad',
    'UNIT_TANK': 'Tank', 'UNIT_SIEGE_TANK': 'Siege Tank', 'UNIT_DEVASTATOR': 'Devastator',
    'UNIT_LAUNCHER': 'Launcher', 'UNIT_SONIC_TANK': 'Sonic Tank',
}


# --- normalising by price -------------------------------------------------
#
# Where the square on the cost comes from, without invoking Lanchester at all.
# Spend the same budget C on each side and let fractional units stand:
#
#     N_1 = C / cost_1                    N_2 = C / cost_2
#     H_1 = N_1 * hp_1   D_1 = N_1 * dps_1        (and the same for side 2)
#
# Side 1 lasts T_1 = H_1 / D_2 under side 2's fire; side 2 lasts T_2 = H_2 / D_1.
# Side 1 is ahead when T_1 > T_2, and cross-multiplying turns that into
#
#     H_1 * D_1  >  H_2 * D_2
#     N_1^2 * hp_1 * dps_1  >  N_2^2 * hp_2 * dps_2
#     C^2 / cost_1^2 * threat_1  >  C^2 / cost_2^2 * threat_2
#     threat_1 / cost_1^2  >  threat_2 / cost_2^2
#
# The budget cancels; the square survives because a squad's threat is quadratic
# in its size -- buying twice as many units doubles the incoming damage the
# squad can absorb AND doubles the outgoing damage, so the product goes up
# fourfold while the bill only doubles.  That is the whole of it.
#
# (Lanchester's square law arrives at the same inequality from a model where
# dying units stop shooting.  The two agree on who wins and differ only on how
# many survivors are left, so nothing here depends on picking one of them.)
#
# threat / cost -- no square -- is the linear-law index: right for duels and for
# fights where nobody can concentrate fire, wrong for a massed battle.  Both are
# reported; they rank the roster very differently.


# --- the model ------------------------------------------------------------

def rate_per_second(unit):
    """Shots per second at full health, from the fire cooldown alone."""
    delay = unit['fireDelay']
    if delay == 0 or unit['damage'] == 0:
        return 0.0
    if unit['firesTwice']:
        ticks = COOLDOWN_MULTIPLIER * delay + FIRE_TWICE_GAP + 2 * COOLDOWN_RANDOM_MEAN
        return 2.0 / (ticks / TICKS_PER_SECOND)
    ticks = COOLDOWN_MULTIPLIER * delay + COOLDOWN_RANDOM_MEAN
    return 1.0 / (ticks / TICKS_PER_SECOND)


def build_rows(units, members, trooper_at_range, tuning=None):
    tuning = tuning or {}
    rows = []
    for name, unit in units.items():
        if not unit['isNormalUnit']:
            continue
        dmg_pct, rate_pct = tuning.get(unit['index'], (100, 100))
        # The engine patches the table at startup, so everything downstream sees
        # the tuned numbers.  Do the same here rather than reading the table raw.
        unit = dict(unit)
        unit['damage'] = tuned_damage(unit['damage'], dmg_pct)
        unit['fireDelay'] = tuned_fire_delay(unit['fireDelay'], unit['firesTwice'],
                                             rate_pct)
        damage, note = effective_damage(name, unit['damage'], trooper_at_range)
        rate = rate_per_second(unit)
        dps = damage * rate
        correction = FIRE_TWICE_LIFETIME_CORRECTION if unit['firesTwice'] else 1.0
        threat = dps * unit['hitpoints'] * correction
        rows.append({
            'name': name,
            'hitpoints': unit['hitpoints'],
            'tableDamage': unit['damage'],
            'tuning': {'damage': dmg_pct, 'rate': rate_pct},
            'damage': damage,
            'damageNote': note,
            'fireDelay': unit['fireDelay'],
            'sonicProtection': unit['sonicProtection'],
            'bulletType': unit['bulletType'],
            'firesTwice': unit['firesTwice'],
            'correction': correction,
            'rate': rate,
            'dps': dps,
            'threat': threat,
            'cost': unit['cost'],
            'costPerThreat': (unit['cost'] / threat) if threat else None,
            'threatPerCredit': (threat / unit['cost']) if unit['cost'] else None,
            'threatPerCredit2': (threat / (unit['cost'] ** 2)) if unit['cost'] else None,
            'class': members.get(name_to_enum(name), None),
        })
    rows.sort(key=lambda r: -r['threat'])
    return rows


def name_to_enum(name):
    for enum_name, display in UNIT_ENUM_TO_NAME.items():
        if display == name:
            return enum_name
    return None


def class_matrix_tables(rows, matrix, order):
    """Per-unit threat against each target class, and the class aggregates."""
    idx = {c: i for i, c in enumerate(order)}
    per_unit = []
    for row in rows:
        cls = row['class']
        if cls is None or row['threat'] == 0:
            continue
        against = {}
        for target in order:
            mult = matrix[idx[cls]][idx[target]] / 100.0
            threat = row['threat'] * mult
            against[target] = {
                'multiplier': mult,
                'threat': threat,
                'threatPerCredit': threat / row['cost'] if row['cost'] else None,
                'threatPerCredit2': threat / (row['cost'] ** 2) if row['cost'] else None,
            }
        per_unit.append({'name': row['name'], 'class': cls,
                         'cost': row['cost'], 'against': against})

    aggregate = {}
    for attacker in order:
        members = [u for u in per_unit if u['class'] == attacker]
        aggregate[attacker] = {}
        for target in order:
            if not members:
                aggregate[attacker][target] = None
                continue
            threats = [m['against'][target]['threat'] for m in members]
            per_credit = [m['against'][target]['threatPerCredit'] for m in members]
            per_credit2 = [m['against'][target]['threatPerCredit2'] for m in members]
            aggregate[attacker][target] = {
                'meanThreat': sum(threats) / len(threats),
                'meanThreatPerCredit': sum(per_credit) / len(per_credit),
                'meanThreatPerCredit2': sum(per_credit2) / len(per_credit2),
            }

    exchange = {'linear': {}, 'square': {}}
    for law, key in (('linear', 'meanThreatPerCredit'), ('square', 'meanThreatPerCredit2')):
        for attacker in order:
            exchange[law][attacker] = {}
            for target in order:
                a = aggregate[attacker][target]
                b = aggregate[target][attacker]
                if not a or not b or not b[key]:
                    exchange[law][attacker][target] = None
                else:
                    exchange[law][attacker][target] = a[key] / b[key]
    return per_unit, aggregate, exchange


# --- explicit squad duels -------------------------------------------------
#
# The same comparison written out rather than reduced: build BUDGET credits of
# each side, print how long each lasts.  It says the same thing as the square-law
# exchange matrix and is far easier to argue with.

BUDGET = 1000.0


def build_squads(rows, per_unit, order):
    """BUDGET credits of each class, split evenly by credits across its members."""
    by_name = {r['name']: r for r in rows}
    squads = {}
    for cls in order:
        members = sorted([u['name'] for u in per_unit if u['class'] == cls],
                         key=lambda n: by_name[n]['cost'])
        if not members:
            continue
        share = BUDGET / len(members)
        counts = {m: share / by_name[m]['cost'] for m in members}
        squads[cls] = {
            'members': members,
            'counts': counts,
            'hitpoints': sum(counts[m] * by_name[m]['hitpoints'] for m in members),
            # threat / hitpoints is the DPS with the firesTwice correction already in
            'dps': sum(counts[m] * by_name[m]['threat'] / by_name[m]['hitpoints']
                       for m in members),
        }
    return squads


def squad_duels(squads, matrix, order):
    """Seconds for each class to be wiped by each other class, and the ratio."""
    idx = {c: i for i, c in enumerate(order)}
    survival = {}
    for victim in order:
        survival[victim] = {}
        for killer in order:
            dps = squads[killer]['dps'] * matrix[idx[killer]][idx[victim]] / 100.0
            survival[victim][killer] = squads[victim]['hitpoints'] / dps if dps else None
    exchange = {}
    for me in order:
        exchange[me] = {}
        for them in order:
            mine, theirs = survival[me][them], survival[them][me]
            exchange[me][them] = (mine / theirs) if mine and theirs else None
    return survival, exchange


def unit_duels(rows, per_unit, matrix, order):
    """The same duel with no class aggregation at all: unit against unit."""
    idx = {c: i for i, c in enumerate(order)}
    by_name = {r['name']: r for r in rows}
    classed = {u['name']: u['class'] for u in per_unit}
    names = [u['name'] for u in sorted(per_unit,
                                       key=lambda u: (order.index(u['class']), -by_name[u['name']]['threat']))]
    table = {}
    for me in names:
        table[me] = {}
        a = by_name[me]
        for them in names:
            b = by_name[them]
            # BUDGET credits of each; the budget cancels, but keep it explicit
            n_a, n_b = BUDGET / a['cost'], BUDGET / b['cost']
            mult_a = matrix[idx[classed[me]]][idx[classed[them]]] / 100.0
            mult_b = matrix[idx[classed[them]]][idx[classed[me]]] / 100.0
            hp_a, hp_b = n_a * a['hitpoints'], n_b * b['hitpoints']
            dps_a = n_a * (a['threat'] / a['hitpoints']) * mult_a
            dps_b = n_b * (b['threat'] / b['hitpoints']) * mult_b
            table[me][them] = (hp_a / dps_b) / (hp_b / dps_a)
    return names, table


# --- output ---------------------------------------------------------------

def print_duels(rows, per_unit, matrix, order):
    squads = build_squads(rows, per_unit, order)
    survival, exchange = squad_duels(squads, matrix, order)

    print('SQUADS FOR %d CREDITS, split evenly by credits inside a class' % BUDGET)
    print('fractional units are kept -- rounding would be a second, unrelated model')
    print()
    head = '%-6s%-46s%8s%10s%10s' % ('class', 'composition', 'units', 'total HP', 'base DPS')
    print(head)
    print('-' * len(head))
    for cls in order:
        sq = squads[cls]
        comp = ', '.join('%s x%.2f' % (m, sq['counts'][m]) for m in sq['members'])
        print('%-6s%-46s%8.2f%10.1f%10.2f'
              % (cls, comp, sum(sq['counts'].values()), sq['hitpoints'], sq['dps']))

    print()
    print('SECONDS TO WIPE THE SQUAD -- row is the side that dies')
    print('%-6s' % '' + ''.join('%11s' % ('by ' + c) for c in order))
    for victim in order:
        cells = ''.join('%11.1f' % survival[victim][k] if survival[victim][k]
                        else '%11s' % '-' for k in order)
        print('%-6s' % victim + cells)

    print()
    print('EXCHANGE: (how long I last) / (how long they last), same money each')
    print('above 1.00 means the row class outlasts the column class')
    print('%-8s' % 'me/them' + ''.join('%9s' % c for c in order))
    for me in order:
        cells = ''.join('%9.2f' % exchange[me][t] if exchange[me][t]
                        else '%9s' % '-' for t in order)
        print('%-8s' % me + cells)

    names, table = unit_duels(rows, per_unit, matrix, order)
    print()
    print('THE SAME DUEL WITHOUT CLASSES: unit against unit, %d credits of each' % BUDGET)
    print('%-14s' % 'me / them' + ''.join('%7s' % n[:6] for n in names))
    for me in names:
        print('%-14s' % me + ''.join('%7.2f' % table[me][t] for t in names))


def print_report(rows, per_unit, aggregate, exchange, matrix, order):
    print('THREAT = DPS x hitpoints x firesTwice correction')
    print('cost/threat is the linear-law price index; threat/cost^2 is the')
    print('square-law one -- see the note above the model.')
    print()
    head = ('%-14s%6s%6s%6s%9s%8s%10s%7s%13s%14s'
            % ('unit', 'HP', 'dmg', 'eff', 'shots/s', 'DPS', 'THREAT', 'cost',
               'cost/threat', 'threat/cost^2'))
    print(head)
    print('-' * len(head))
    for r in rows:
        cpt = '%.3f' % r['costPerThreat'] if r['costPerThreat'] else '-'
        sq = '%.5f' % r['threatPerCredit2'] if r['threatPerCredit2'] else '-'
        print('%-14s%6d%6d%6d%9.3f%8.2f%10.1f%7d%13s%14s'
              % (r['name'], r['hitpoints'], r['tableDamage'], r['damage'],
                 r['rate'], r['dps'], r['threat'], r['cost'], cpt, sq))

    print()
    print('DAMAGE MULTIPLIER MATRIX (percent, from s_combatBalance)')
    print('%-8s' % 'att\\tgt' + ''.join('%7s' % c for c in order))
    for i, attacker in enumerate(order):
        print('%-8s' % attacker + ''.join('%7d' % matrix[i][j] for j in range(len(order))))

    print()
    print('THREAT PER 1000 CREDITS, unit versus target class (linear law)')
    print('%-14s%-4s' % ('unit', 'cls') + ''.join('%9s' % c for c in order))
    for u in sorted(per_unit, key=lambda u: (order.index(u['class']), u['name'])):
        cells = ''.join('%9.1f' % (u['against'][c]['threatPerCredit'] * 1000) for c in order)
        print('%-14s%-4s' % (u['name'], u['class']) + cells)

    print()
    print('CLASS AGGREGATE: mean threat per 1000 credits (linear law)')
    print('%-8s' % 'att\\tgt' + ''.join('%9s' % c for c in order))
    for attacker in order:
        cells = ''
        for target in order:
            a = aggregate[attacker][target]
            cells += '%9.1f' % (a['meanThreatPerCredit'] * 1000) if a else '%9s' % '-'
        print('%-8s' % attacker + cells)

    for law, title in (('linear', 'LINEAR LAW (threat / cost) -- duels, no concentration of fire'),
                       ('square', 'SQUARE LAW (threat / cost^2) -- massed battle, everyone shoots')):
        print()
        print('EXCHANGE RATE, %s' % title)
        print('above 1.00 means the row class wins the credit-for-credit trade')
        print('%-8s' % 'att\\tgt' + ''.join('%9s' % c for c in order))
        for attacker in order:
            cells = ''
            for target in order:
                e = exchange[law][attacker][target]
                cells += '%9.2f' % e if e else '%9s' % '-'
            print('%-8s' % attacker + cells)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--json', action='store_true', help='dump the model as JSON')
    ap.add_argument('--trooper-range', action='store_true',
                    help='rocket infantry firing beyond two tiles (missile, -25%%)')
    ap.add_argument('--flat', action='store_true',
                    help='force every multiplier to 100, to see what the base '
                         'stats alone produce')
    ap.add_argument('--duel', action='store_true',
                    help='explicit %d-credit squad duels instead of the indices' % BUDGET)
    ap.add_argument('--no-tuning', action='store_true',
                    help='ignore the per-unit s_unitTuning table, to see what '
                         'Westwood shipped')
    ap.add_argument('--root', default=ROOT, help='repository root')
    args = ap.parse_args()

    units = parse_unitinfo(os.path.join(args.root, 'src/table/unitinfo.c'))
    unit_c = os.path.join(args.root, 'src/unit.c')
    matrix = parse_class_matrix(unit_c)
    members = parse_class_members(unit_c)
    order = parse_class_order(unit_c)
    enum = parse_unit_enum(os.path.join(args.root, 'src/unit.h'))
    tuning = {} if args.no_tuning else parse_unit_tuning(unit_c, enum)
    if args.flat:
        matrix = [[100] * len(order) for _ in order]

    rows = build_rows(units, members, args.trooper_range, tuning)
    per_unit, aggregate, exchange = class_matrix_tables(rows, matrix, order)

    if args.json:
        squads = build_squads(rows, per_unit, order)
        survival, duel = squad_duels(squads, matrix, order)
        names, unit_table = unit_duels(rows, per_unit, matrix, order)
        json.dump({'classes': order, 'matrix': matrix, 'units': rows,
                   'perUnit': per_unit, 'aggregate': aggregate, 'exchange': exchange,
                   'budget': BUDGET, 'squads': squads, 'survival': survival,
                   'duelExchange': duel, 'unitDuelOrder': names,
                   'unitDuel': unit_table},
                  sys.stdout, indent=2)
        print()
    elif args.duel:
        print_duels(rows, per_unit, matrix, order)
    else:
        print_report(rows, per_unit, aggregate, exchange, matrix, order)


if __name__ == '__main__':
    main()
