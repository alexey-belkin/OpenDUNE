#!/usr/bin/env python3
"""Attrition duels for OpenDUNE — the honest version of the threat matrix.

threat_report.py reduces a fight to one closed-form number. This does not: it
buys two squads for a budget, then plays the fight out shot by shot. Units are
whole, they die one at a time, and their firepower leaves the field with them.

What that buys over the closed form:

  * **Overkill is wasted.** A Devastator's 40-damage shot on a 20-hitpoint
    Soldier throws half of itself away. The square law cannot see this at all,
    and it is the largest single correction the simulation makes -- it runs
    against big units, not for them.
  * **firesTwice is exact.** The 0.75 lifetime correction in the closed form is
    an average; here a unit simply loses its second shot the moment it drops
    below half health, which is what script/unit.c:673 does.
  * **The pool cap can be switched on.** This fork gives every house 90 ground
    slots (units.md), so 10000 credits of Soldier is not 166 units, it is 90.
    It is off by default: once the cap binds, extra money stops buying army and
    a money-for-money exchange rate stops meaning anything.

Modelling choices that are choices, not facts -- change them and the numbers
move:

  * Focus fire. Both sides shoot one target at a time, picked by highest
    damage-per-hitpoint, ties going to the most wounded. Spreading fire instead
    favours whoever has more targets.
  * Cooldowns use the mean of the engine's 0-or-1 random term rather than
    rolling it, so a run is reproducible.
  * A unit's first shot comes one cooldown after contact, not instantly, so no
    volley is fired that was not paid for.
  * Range, movement, arrival order and terrain do not exist. This is two squads
    already in contact.

Usage:
    python3 tools/threat_attrition.py                    # class matrix
    python3 tools/threat_attrition.py --units            # unit-vs-unit matrix
    python3 tools/threat_attrition.py --budget 10000
    python3 tools/threat_attrition.py --cap              # apply the 90-slot pool
    python3 tools/threat_attrition.py --json
"""

import argparse
import heapq
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import threat_report as tr

ROOT = tr.ROOT

# This fork's ground band is 180 slots, 90 to a house (units.md).
POOL_CAP = 90


class UnitType(object):
    """Everything the simulation needs about one buildable type."""

    def __init__(self, row, cls):
        self.name = row['name']
        self.cls = cls
        self.cost = row['cost']
        self.maxhp = row['hitpoints']
        self.damage = row['damage']          # per shot, already engine-effective
        self.firesTwice = row['firesTwice']
        delay = row['fireDelay']
        # script/unit.c:730 -- the counter ticks at 20 Hz (unit.c:3035)
        self.cooldown = (2 * delay + tr.COOLDOWN_RANDOM_MEAN) / tr.TICKS_PER_SECOND
        # script/unit.c:734 -- the short gap inside a doublet
        self.shortCooldown = (tr.FIRE_TWICE_GAP + tr.COOLDOWN_RANDOM_MEAN) / tr.TICKS_PER_SECOND
        # static kill priority: threat removed per hitpoint spent getting there
        self.priority = (row['dps'] / self.maxhp) if self.maxhp else 0.0
        # The Sonic Tank does not fire a shot, it sends a beam down a line of
        # tiles, dealing hitpoints/4 + 1 in each and losing one hitpoint per tile
        # (unit.c:4504).  Everything standing on that line takes a hit, so its
        # damage is not per-target but per-formation -- see beam_damage().
        self.beam = row['bulletType'] == 'UNIT_SONIC_BLAST'
        self.beamStart = row['tableDamage'] if self.beam else 0
        # unit.c:4508 -- the beam passes straight through anything carrying
        # sonicProtection, which in this table is the Sonic Tank alone.
        self.sonicProof = row['sonicProtection']


def beam_damage(start, tiles):
    """What one sonic blast deals to each successive tile it crosses."""
    out = []
    hp = start
    for _ in range(tiles):
        if hp <= 0:
            break
        out.append(hp // 4 + 1)
        hp -= 1
    return out


def load_types(trooper_at_range=False, flat=False, no_tuning=False):
    unit_c = os.path.join(ROOT, 'src/unit.c')
    units = tr.parse_unitinfo(os.path.join(ROOT, 'src/table/unitinfo.c'))
    matrix = tr.parse_class_matrix(unit_c)
    members = tr.parse_class_members(unit_c)
    order = tr.parse_class_order(unit_c)
    enum = tr.parse_unit_enum(os.path.join(ROOT, 'src/unit.h'))
    tuning = {} if no_tuning else tr.parse_unit_tuning(unit_c, enum)
    if flat:
        matrix = [[100] * len(order) for _ in order]
    rows = tr.build_rows(units, members, trooper_at_range, tuning)
    by_name = {}
    for row in rows:
        if not row['threat'] or row['class'] is None:
            continue
        by_name[row['name']] = UnitType(row, row['class'])
    return by_name, matrix, order


def buy(composition, budget, cap):
    """Whole units for a budget, split evenly by credits across the types.

    Returns the roster and what the pool cap and the rounding actually cost, so
    a result that was decided by the cap rather than by the fight is visible.
    """
    share = budget / float(len(composition))
    roster = []
    for t in composition:
        roster.extend([t] * int(share // t.cost))
    wanted = len(roster)
    if cap and wanted > cap:
        # Drop from the back of the cheapest types first: the cap is a slot
        # limit, and a player short of slots spends them on the better unit.
        roster.sort(key=lambda t: -t.cost)
        roster = roster[:cap]
    return roster, wanted


class Side(object):
    def __init__(self, roster, label):
        self.label = label
        # type, hitpoints, fireTwiceFlip, seconds on the field
        self.alive = [[t, t.maxhp, False, 0.0] for t in roster]
        self.spent = sum(t.cost for t in roster)

    def target(self, spread_key=None):
        """Who to shoot.

        Focus (default): best damage-per-hitpoint, ties to the most wounded --
        the fastest way to take firepower off the field.  Spread: shooters fan
        out across the survivors, which is what an army without perfect target
        assignment actually does.  The two bracket the truth, and the doublet
        correction below depends on which one you assume.
        """
        living = [u for u in self.alive if u[1] > 0]
        if not living:
            return None
        if spread_key is not None:
            return living[spread_key % len(living)]
        best = None
        for u in living:
            key = (u[0].priority, -u[1])
            if best is None or key > best[0]:
                best = (key, u)
        return best[1]

    def value(self):
        return sum(u[0].cost for u in self.alive if u[1] > 0)


def simulate(rosterA, rosterB, matrix, order, limit=6000.0, waste=True,
             spread=False, tally=None, sonic_tiles=1):
    """Play the fight out. Returns (winner, survivors A, survivors B, seconds).

    tally, if given, collects per-type {shots, aliveSeconds} so the doublet loss
    can be measured rather than assumed.
    """
    idx = {c: i for i, c in enumerate(order)}
    A, B = Side(rosterA, 'A'), Side(rosterB, 'B')

    events = []          # (time, seq, side, unit)
    seq = 0
    born = {}
    for side in (A, B):
        for u in side.alive:
            # First shot is earned, not free: a unit acquires and reloads before
            # it fires.  Opening everyone at t=0 would hand both sides a volley
            # nobody paid a cooldown for, and would bias any rate measured here.
            heapq.heappush(events, (u[0].cooldown, seq, side, u))
            born[id(u)] = seq
            seq += 1

    now = 0.0
    while events:
        now, _, side, shooter = heapq.heappop(events)
        if now > limit:
            break
        if shooter[1] <= 0:                      # died before this shot landed
            continue
        foe = B if side is A else A
        victim = foe.target(born.get(id(shooter)) if spread else None)
        if victim is None:
            break

        t = shooter[0]
        if t.beam:
            # One beam, several tiles.  How many of them are occupied is a
            # property of the enemy's formation, not of the Sonic Tank, so it is
            # a parameter: sonic_tiles=1 is a single target in the open,
            # sonic_tiles=10 is a perfect line and the beam's full range.
            hit = 0
            for step in beam_damage(t.beamStart, sonic_tiles):
                if victim is None:
                    break
                if not victim[0].sonicProof:
                    victim[1] -= step * matrix[idx[t.cls]][idx[victim[0].cls]] / 100.0
                    if victim[1] <= 0:
                        victim[3] = now
                hit += 1
                nxt = foe.target(born.get(id(shooter)) + hit if spread else None)
                victim = nxt if nxt is not victim else foe.target(hit)
        else:
            left = t.damage * matrix[idx[t.cls]][idx[victim[0].cls]] / 100.0
            victim[1] -= left
            if victim[1] <= 0:
                victim[3] = now
        if not waste:
            # Diagnostic mode: spill the excess onto the next target instead of
            # losing it, which is what the closed form implicitly assumes.
            # The spill is re-scaled if the next target is of another class.
            while victim[1] < 0:
                spill = -victim[1] / (matrix[idx[t.cls]][idx[victim[0].cls]] / 100.0)
                nxt = foe.target(None)
                if nxt is None or nxt is victim:
                    break
                victim = nxt
                victim[1] -= spill * matrix[idx[t.cls]][idx[victim[0].cls]] / 100.0
                if victim[1] <= 0:
                    victim[3] = now

        # script/unit.c:673 -- the doublet is gated on being above half health
        if t.firesTwice and shooter[1] > t.maxhp / 2.0:
            shooter[2] = not shooter[2]
            gap = t.shortCooldown if shooter[2] else t.cooldown
        else:
            shooter[2] = False
            gap = t.cooldown

        if tally is not None:
            # Counting the gap a shot earned, not elapsed time, makes this exact:
            # it cannot be skewed by the fight ending mid-reload.
            rec = tally.setdefault(t.name, {'shots': 0, 'armed': 0, 'type': t})
            rec['shots'] += 1
            if t.firesTwice and shooter[1] > t.maxhp / 2.0:
                rec['armed'] += 1

        seq += 1
        heapq.heappush(events, (now + gap, seq, side, shooter))

        if not any(u[1] > 0 for u in foe.alive):
            break

    aliveA = sum(1 for u in A.alive if u[1] > 0)
    aliveB = sum(1 for u in B.alive if u[1] > 0)
    if aliveA and not aliveB:
        winner = 'A'
    elif aliveB and not aliveA:
        winner = 'B'
    else:
        winner = 'A' if A.value() >= B.value() else 'B'
    return winner, A, B, now


def fair_budget(compA, compB, budget, matrix, order, cap, lo=0.05, hi=20.0, steps=22,
                waste=True, spread=False, sonic_tiles=1):
    """How much money B needs to hold A's budget to a draw.

    Above 1.00 means A is the stronger buy: B has to outspend it to survive.
    """
    for _ in range(steps):
        mid = (lo * hi) ** 0.5                       # bisect in log space
        rosterA, _ = buy(compA, budget, cap)
        rosterB, _ = buy(compB, budget * mid, cap)
        winner, _, _, _ = simulate(rosterA, rosterB, matrix, order, waste=waste,
                                   spread=spread, sonic_tiles=sonic_tiles)
        if winner == 'A':
            lo = mid                                  # B still loses, give it more
        else:
            hi = mid
    return (lo * hi) ** 0.5


def doublet_report(types, matrix, order, budget, cap, spread):
    """Measure what firesTwice is actually worth, instead of assuming 0.75.

    The closed form multiplies a firesTwice unit's threat by 0.75, reasoning
    that it spends the top half of its health bar firing doublets and the bottom
    half firing singles.  That is exactly right for a unit whose damage arrives
    evenly over its whole life.  It is wrong for a unit that is ignored and then
    focused down, because such a unit spends almost no time wounded.  So the
    number is not a constant -- it depends on how fire is distributed, and this
    measures it at both ends.
    """
    comps = {c: sorted([t for t in types.values() if t.cls == c], key=lambda t: t.cost)
             for c in order}
    tally = {}
    for a in order:
        for b in order:
            if a == b:
                continue
            ra, _ = buy(comps[a], budget, cap)
            rb, _ = buy(comps[b], budget, cap)
            simulate(ra, rb, matrix, order, spread=spread, tally=tally)

    out = []
    for name in sorted(tally):
        rec = tally[name]
        t = rec['type']
        if not rec['shots']:
            continue
        # Share of its shooting the unit did while still above half health, i.e.
        # while the doublet was available.  Bounded [0, 1] by construction, and
        # free of the censoring that skews an average of earned cooldowns: a unit
        # that draws the short gap is likelier to fire again before it dies.
        armed = rec['armed'] / float(rec['shots'])
        single = 1.0 / t.cooldown
        double = 2.0 / (t.shortCooldown + t.cooldown)
        # rate it actually sustained, and that as a fraction of the full doublet
        rate = armed * double + (1.0 - armed) * single
        out.append((name, t.firesTwice, rec['shots'], armed, rate, rate / double))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--budget', type=float, default=10000.0)
    ap.add_argument('--units', action='store_true', help='unit-vs-unit instead of class')
    ap.add_argument('--cap', action='store_true',
                    help='apply this fork\'s 90-slot ground pool per house')
    ap.add_argument('--flat', action='store_true', help='every class multiplier forced to 100')
    ap.add_argument('--no-overkill', action='store_true',
                    help='diagnostic: spill excess damage onto the next target '
                         'instead of losing it, to isolate what overkill costs')
    ap.add_argument('--trooper-range', action='store_true')
    ap.add_argument('--no-tuning', action='store_true',
                    help='ignore the per-unit s_unitTuning table in unit.c')
    ap.add_argument('--spread', action='store_true',
                    help='shooters fan out across survivors instead of focusing')
    ap.add_argument('--sonic-tiles', type=int, default=1, metavar='N',
                    help='occupied tiles a sonic beam crosses: 1 is a lone '
                         'target, 10 is a perfect line at full beam range')
    ap.add_argument('--split', action='append', default=[], metavar='CLASS',
                    help='break this class into one group per unit type, e.g. '
                         '--split AR to separate the Launcher from the Sonic Tank')
    ap.add_argument('--doublet', action='store_true',
                    help='measure what firesTwice is worth, replacing the 0.75 guess')
    ap.add_argument('--json', action='store_true')
    args = ap.parse_args()

    types, matrix, order = load_types(args.trooper_range, args.flat, args.no_tuning)
    cap = POOL_CAP if args.cap else None
    budget = args.budget

    if args.doublet:
        print('WHAT firesTwice IS ACTUALLY WORTH')
        print('measured over every class-versus-class fight at %d credits a side'
              % budget)
        print('the closed form assumes a flat 0.75 for every one of these')
        print('armed % is the share of its shooting done above half health')
        print()
        for label, spread in (('focus fire', False), ('spread fire', True)):
            print('  --- %s ---' % label)
            print('  %-14s%8s%9s%11s%10s%12s'
                  % ('unit', 'doublet', 'shots', 'armed %', 'shots/s', 'vs full x2'))
            for name, twice, shots, armed, rate, ratio in doublet_report(
                    types, matrix, order, budget, cap, spread):
                if not twice:
                    continue
                print('  %-14s%8s%9d%11.1f%10.4f%12.3f'
                      % (name, 'yes', shots, 100 * armed, rate, ratio))
            print()
        return

    if args.units:
        names = sorted(types, key=lambda n: (order.index(types[n].cls), -types[n].cost))
        comps = {n: [types[n]] for n in names}
    else:
        names, comps = [], {}
        for c in order:
            members = sorted([t for t in types.values() if t.cls == c],
                             key=lambda t: t.cost)
            if c in args.split:
                # Same class for the damage matrix, separate groups for the
                # arithmetic: the multipliers still say AR, but a Launcher and a
                # Sonic Tank stop being averaged into one imaginary unit.
                for t in members:
                    names.append(t.name)
                    comps[t.name] = [t]
            else:
                names.append(c)
                comps[c] = members

    rosters = {}
    for n in names:
        roster, wanted = buy(comps[n], budget, cap)
        rosters[n] = {
            'roster': roster,
            'wanted': wanted,
            'count': len(roster),
            'capped': wanted > len(roster),
            'hitpoints': sum(t.maxhp for t in roster),
            'spent': sum(t.cost for t in roster),
        }

    exchange = {a: {} for a in names}
    detail = {a: {} for a in names}
    for i, a in enumerate(names):
        for j, b in enumerate(names):
            if a == b:
                exchange[a][b] = 1.0
                continue
            if j < i:
                exchange[a][b] = 1.0 / exchange[b][a]
            else:
                waste = not args.no_overkill
                forward = fair_budget(comps[a], comps[b], budget, matrix, order, cap,
                                      waste=waste, spread=args.spread,
                                      sonic_tiles=args.sonic_tiles)
                reverse = fair_budget(comps[b], comps[a], budget, matrix, order, cap,
                                      waste=waste, spread=args.spread,
                                      sonic_tiles=args.sonic_tiles)
                # whole units land the two searches on slightly different steps;
                # the geometric mean is the midpoint of that granularity
                exchange[a][b] = (forward / reverse) ** 0.5
            if not args.units:
                w, A, B, t = simulate(rosters[a]['roster'], rosters[b]['roster'],
                                      matrix, order, spread=args.spread,
                                      sonic_tiles=args.sonic_tiles)
                detail[a][b] = {
                    'winner': a if w == 'A' else b,
                    'survivorsA': sum(1 for u in A.alive if u[1] > 0),
                    'survivorsB': sum(1 for u in B.alive if u[1] > 0),
                    'valueA': A.value(), 'valueB': B.value(),
                    'seconds': round(t, 1),
                }

    if args.json:
        json.dump({'budget': budget, 'cap': cap, 'names': names,
                   'squads': {n: {k: v for k, v in rosters[n].items() if k != 'roster'}
                              for n in names},
                   'exchange': exchange, 'detail': detail}, sys.stdout, indent=2)
        print()
        return

    print('ATTRITION DUELS -- %d credits a side, whole units, shot by shot'
          % budget + ('' if cap is None else ', %d-slot ground pool' % cap))
    print()
    head = '%-14s%8s%8s%10s%12s' % ('squad', 'bought', 'wanted', 'total HP', 'credits used')
    print(head)
    print('-' * len(head))
    for n in names:
        r = rosters[n]
        print('%-14s%8d%8d%10d%12d%s'
              % (n, r['count'], r['wanted'], r['hitpoints'], r['spent'],
                 '   <- pool cap' if r['capped'] else ''))

    print()
    print('FAIR BUDGET: how many credits the column side needs per 1 credit of the row side')
    print('above 1.00 means the row squad is the better buy')
    w = 8 if args.units else 9
    print('%-14s' % 'row / column' + ''.join(('%' + str(w) + 's') % n[:w - 1] for n in names))
    for a in names:
        print('%-14s' % a + ''.join(('%' + str(w) + '.2f') % exchange[a][b] for b in names))

    if not args.units:
        print()
        print('EQUAL MONEY, HEAD TO HEAD: who is left standing at %d vs %d' % (budget, budget))
        print('%-8s%-8s%10s%10s%10s' % ('row', 'column', 'winner', 'survivors', 'seconds'))
        for a in names:
            for b in names:
                if a == b:
                    continue
                d = detail[a][b]
                if d['winner'] != a:
                    continue
                print('%-8s%-8s%10s%10d%10.1f'
                      % (a, b, d['winner'],
                         d['survivorsA'] if d['winner'] == a else d['survivorsB'],
                         d['seconds']))


if __name__ == '__main__':
    main()
