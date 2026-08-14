#!/usr/bin/env python3
"""Build telemetry.html from the recorded matches in bin/telemetry/.

A page opened from the filesystem cannot list a directory and cannot fetch a
sibling file -- browsers block both.  So the data is baked in: this reads every
recording, keeps the metadata and the samples, and writes one self-contained
page with a match picker.  Re-run it after recording new matches.

    ./opendune --skirmish=ordos,atreides --war=0,0,8919 --war-telemetry=200000,5000
    python3 tools/telemetry_report.py

Usage: telemetry_report.py [telemetry-dir] [output.html]
"""

import json
import os
import sys
import datetime

DEFAULT_DIR = os.path.join("bin", "telemetry")
DEFAULT_OUT = "telemetry.html"

# Columns as written by WarSearch_RunTelemetry(), minus tick and house.
COLUMNS = ["refineries", "combatStructures", "harvesters", "combatUnits",
           "combatHitpoints", "damageTaken", "spiceRefined", "credits", "powerSurplus",
           "shotsTurret", "shotsStructure", "shotsUnit", "shotsBypass",
           "idleAttackers", "idleOnWave", "stalledHarvesters", "freeRefineries",
           "wavePhase", "waveUnits", "waveAtLD", "waveColumn",
           "wavesLaunched", "wavesAborted",
           "builtArt", "builtAss", "builtRaid", "builtGar",
           "turretZone", "harvLost", "harvKilled"]


def read_match(path):
    """Parse one recording into {meta, ticks, series} or None if unusable."""
    meta, header, rows = {}, None, []

    with open(path, encoding="utf-8") as fp:
        for line in fp:
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
                rows.append(cells)

    if not rows or header is None:
        return None

    houses = meta.get("houses", "A,B").split(",")
    ticks = sorted({int(r[0]) for r in rows})
    index = {h: {} for h in houses}
    for r in rows:
        if r[1] in index:
            index[r[1]][int(r[0])] = r

    # A house missing a sample at some tick means a truncated file; drop it
    # rather than plotting a series with holes in it.
    for h in houses:
        if any(t not in index[h] for t in ticks):
            return None

    def column(house, name):
        col = header.index(name)
        return [int(index[house][t][col]) for t in ticks]

    def deltas(values):
        return [0] + [max(0, values[i] - values[i - 1]) for i in range(1, len(values))]

    series = {}
    for i, h in enumerate(houses):
        other = houses[1 - i] if len(houses) == 2 else h
        series[h] = {name: column(h, name) for name in COLUMNS}
        # Nobody records who fired, so "dealt" is what the other side took.
        series[h]["damageDealt"] = deltas(column(other, "damageTaken"))
        series[h]["spiceDelta"] = deltas(series[h]["spiceRefined"])

    stamp = meta.get("recorded", "")
    try:
        when = datetime.datetime.strptime(stamp.strip(), "%a %b %d %H:%M:%S %Y")
        recorded = when.strftime("%d.%m.%Y %H:%M")
        sortkey = when.timestamp()
    except ValueError:
        recorded, sortkey = stamp.strip(), 0.0

    values = [int(v) for v in meta.get("value", "0,0").split(",")]
    shares = [int(v) for v in meta.get("shares", "0,0").split(",")]
    late = [int(v) for v in meta.get("shareLate", "90,90").split(",")]
    wipe = [int(v) for v in meta.get("wipeout", "0,0").split(",")]

    return {
        "file": os.path.basename(path),
        "houses": houses,
        "recorded": recorded,
        "sortkey": sortkey,
        "winner": meta.get("winner", "?"),
        "values": values,
        "shares": shares,
        "shareLate": late,
        "switchTick": int(meta.get("switchTick", 0)),
        "seed": int(meta.get("seed", 0)),
        "doctrine": meta.get("doctrine", "A,A").split(","),
        "ticks": int(meta.get("ticks", ticks[-1])),
        "step": int(meta.get("step", 0)),
        "wipeout": wipe,
        "endTick": ticks[-1],
        "samples": ticks,
        "series": series,
    }


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DIR
    output = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OUT

    if not os.path.isdir(directory):
        sys.exit("no telemetry directory at %s -- record a match first" % directory)

    matches, skipped = [], []
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".csv"):
            continue
        parsed = read_match(os.path.join(directory, name))
        if parsed is None:
            skipped.append(name)
        else:
            matches.append(parsed)

    if not matches:
        sys.exit("no readable recordings in %s" % directory)

    matches.sort(key=lambda m: (-m["sortkey"], m["file"]))

    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "telemetry_report.html"), encoding="utf-8") as fp:
        template = fp.read()

    page = template.replace("__MATCHES__", json.dumps(matches, separators=(",", ":")))
    page = page.replace("__GENERATED__", datetime.datetime.now().strftime("%d.%m.%Y %H:%M"))

    with open(output, "w", encoding="utf-8") as fp:
        fp.write(page)

    print("%s: %d matches from %s" % (output, len(matches), directory))
    for name in skipped:
        print("  skipped (truncated or unreadable): %s" % name)


if __name__ == "__main__":
    main()
