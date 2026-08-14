#!/usr/bin/env python3
"""Judge an AI attack wave against four pass/fail criteria.

The point of these is that they cannot be satisfied by accident.  Every one of
them measures the thing directly rather than a proxy that moves for other
reasons -- which is how "the army fights better now" got claimed three times
while units walked past turrets.

    cd bin && ./opendune --war=60,60,1234 --war-telemetry=200000,5000
    python3 tools/wave_check.py

Usage: wave_check.py [telemetry-dir]
"""

import glob
import os
import sys

DIR = sys.argv[1] if len(sys.argv) > 1 else os.path.join("bin", "telemetry")

# name, target, what it means
CRITERIA = [
    ("turret share",  99.0, "shots at turrets, as a share of all shots at structures"),
    ("unit share",    50.0, "shots at units, as a share of all shots"),
    ("idle attackers", 0.0, "combat units on a wave with no target and nowhere to go"),
    ("stalled harv",   0.0, "loaded harvesters standing still while a refinery is free"),
]


def read(path):
    meta, header, rows = {}, None, []
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if line.startswith("#"):
            parts = line[1:].strip().split(" ", 1)
            if len(parts) == 2:
                meta[parts[0]] = parts[1]
            continue
        if header is None:
            header = line.split(",")
            continue
        cells = line.split(",")
        if len(cells) == len(header):
            rows.append(dict(zip(header, cells)))
    return meta, rows


def report(path):
    meta, rows = read(path)
    if not rows:
        return None

    houses = meta.get("houses", "A,B").split(",")
    out = []

    for h in houses:
        mine = [r for r in rows if r["house"] == h]
        if not mine:
            continue
        last = mine[-1]

        turret = int(last.get("shotsTurret", 0))
        struct = int(last.get("shotsStructure", 0))
        unit = int(last.get("shotsUnit", 0))
        bypass = int(last.get("shotsBypass", 0))

        # 1. Turrets are always on the way in, so nearly every shot at a
        #    structure should be at one of them until the line is down.
        atStructures = turret + struct
        turretShare = 100.0 * turret / atStructures if atStructures else None

        # 2. Enemy units are on the path as well.  Shooting buildings more often
        #    than units means the wave walked past what was in front of it.
        allShots = atStructures + unit
        unitShare = 100.0 * unit / allShots if allShots else None

        # 3/4. Levels: worst sample, and how much of the match was bad.
        idle = [int(r.get("idleOnWave", 0)) for r in mine]
        stalled = [(int(r.get("stalledHarvesters", 0)), int(r.get("freeRefineries", 0)))
                   for r in mine]
        # Only counts when a refinery was actually standing free.
        wasted = [s for s, free in stalled if free > 0]

        out.append({
            "house": h,
            "shots": (turret, struct, unit, bypass),
            "turretShare": turretShare,
            "unitShare": unitShare,
            "idleMax": max(idle) if idle else 0,
            "idleBadSamples": sum(1 for v in idle if v > 0),
            "stalledMax": max(wasted) if wasted else 0,
            "stalledBadSamples": sum(1 for v in wasted if v > 0),
            "samples": len(mine),
        })

    return meta, out


def main():
    files = sorted(glob.glob(os.path.join(DIR, "*.csv")))
    if not files:
        sys.exit("no recordings in %s" % DIR)

    verdicts = []

    for path in files:
        parsed = report(path)
        if parsed is None:
            continue
        meta, houses = parsed
        print("%s  seed %s  shares %s" % (os.path.basename(path),
                                          meta.get("seed", "?"), meta.get("shares", "?")))
        for r in houses:
            t, s, u, byp = r["shots"]
            ts = "n/a" if r["turretShare"] is None else "%.1f%%" % r["turretShare"]
            us = "n/a" if r["unitShare"] is None else "%.1f%%" % r["unitShare"]
            print("  %-10s shots turret=%-6d other=%-6d unit=%-6d  of which past a turret in reach: %d" % (r["house"], t, s, u, byp))
            print("             turret share %-7s (target >=99%%)   unit share %-7s (target >=50%%)"
                  % (ts, us))
            print("             idle attackers max %-4d bad samples %d/%d   stalled harv max %-4d bad samples %d/%d"
                  % (r["idleMax"], r["idleBadSamples"], r["samples"],
                     r["stalledMax"], r["stalledBadSamples"], r["samples"]))
            verdicts.append(r)
        print()

    if not verdicts:
        return

    def avg(key):
        vals = [v[key] for v in verdicts if v[key] is not None]
        return sum(vals) / len(vals) if vals else None

    ts, us = avg("turretShare"), avg("unitShare")
    idleBad = sum(v["idleBadSamples"] for v in verdicts)
    stalledBad = sum(v["stalledBadSamples"] for v in verdicts)
    samples = sum(v["samples"] for v in verdicts)

    print("VERDICT over %d house-runs" % len(verdicts))
    print("  1 turret share      %s   %s" % (
        "n/a" if ts is None else "%.1f%%" % ts,
        "PASS" if ts is not None and ts >= 99 else "FAIL"))
    print("  2 unit share        %s   %s" % (
        "n/a" if us is None else "%.1f%%" % us,
        "PASS" if us is not None and us >= 50 else "FAIL"))
    print("  3 idle attackers    %d/%d samples   %s" % (
        idleBad, samples, "PASS" if idleBad == 0 else "FAIL"))
    print("  4 stalled harvest   %d/%d samples   %s" % (
        stalledBad, samples, "PASS" if stalledBad == 0 else "FAIL"))


if __name__ == "__main__":
    main()
