#!/usr/bin/env python3
"""Disassemble the original Dune II EMC scripts (UNIT.EMC, BUILD.EMC, TEAM.EMC).

The legacy behaviour of units, structures and teams is not implemented in C:
OpenDUNE runs the original bytecode and only provides the opcodes it calls.
This tool turns that bytecode back into something readable, so a change to the
supporting C code can be checked against what the script actually does.

Usage:
    tools/dis_emc.py <DUNE.PAK> --list
    tools/dis_emc.py <DUNE.PAK> UNIT.EMC                 # entry offsets
    tools/dis_emc.py <DUNE.PAK> UNIT.EMC --type 16       # one object type
    tools/dis_emc.py <DUNE.PAK> UNIT.EMC --range 877 975 # one subroutine

Opcode semantics follow Script_Run() in src/script/script.c; the function names
follow g_scriptFunctions* in the same file.  Note that SCRIPT_JUMP_NE jumps when
the popped value is *zero*, so it is printed as JUMP_IF_ZERO.
"""

import argparse
import struct
import sys

OPS = {
    0: 'JUMP', 1: 'SETRET', 2: 'PUSHRET/LOC', 3: 'PUSH', 4: 'PUSH2',
    5: 'PUSHVAR', 6: 'PUSHLOCAL', 7: 'PUSHPARAM', 8: 'POPRET/LOC',
    9: 'POPVAR', 10: 'POPLOCAL', 11: 'POPPARAM', 12: 'STACKREWIND',
    13: 'STACKFORWARD', 14: 'CALL', 15: 'JUMP_IF_ZERO', 16: 'UNARY',
    17: 'BINARY', 18: 'RETURN',
}

BINARY = ['&&', '||', '==', '!=', '<', '<=', '>', '>=',
          '+', '-', '*', '/', '>>', '<<', '&', '|', '%', '^']

UNIT_FN = [
    'GetInfo', 'SetAction', 'DisplayText', 'GetDistanceToTile', 'StartAnimation',
    'SetDestination', 'GetOrientation', 'SetOrientation', 'Fire', 'MCVDeploy',
    'SetActionDefault', 'Blink', 'CalculateRoute', 'IsEnemy', 'ExplosionSingle',
    'Die', 'Delay', 'IsFriendly', 'ExplosionMultiple', 'SetSprite',
    'TransportDeliver', 'nop', 'MoveToTarget', 'RandomRange', 'FindIdle',
    'SetDestinationDirect', 'Stop', 'SetSpeed', 'FindBestTarget',
    'GetTargetPriority', 'MoveToStructure', 'IsInTransport', 'GetAmount',
    'RandomSoldier', 'Pickup', 'CallUnitByType', 'ClearCarryallLink',
    'FindStructure', 'VoicePlay', 'DisplayDestroyedText', 'RemoveFog',
    'SearchSpice', 'Harvest', 'nop', 'GetLinkedUnitType', 'GetIndexType',
    'DecodeIndex', 'IsValidDestination', 'GetRandomTile', 'IdleAction',
    'UnitCount', 'GoToClosestStructure', 'nop', 'nop', 'Sandworm_GetBestTarget',
    'Unknown2BD5', 'GetOrientation2', 'nop', 'SetTarget', 'Unknown0288',
    'DelayRandom', 'Rotate', 'GetDistanceToObject', 'nop',
]

STRUCTURE_FN = [
    'Delay', 'nop', 'Unknown0A81', 'FindUnitByType', 'SetState', 'DisplayText',
    'Unknown11B9', 'Unknown0C5A', 'FindTargetUnit', 'RotateTurret',
    'GetDirection', 'Fire', 'nop', 'GetState', 'VoicePlay', 'RemoveFogAroundTile',
] + ['nop'] * 5 + ['RefineSpice', 'Explode', 'Destroy', 'nop']

FUNCTIONS = {'UNIT.EMC': UNIT_FN, 'BUILD.EMC': STRUCTURE_FN}


def read_pak(path):
    """Yield (name, payload) for every file in a Westwood PAK archive."""
    blob = open(path, 'rb').read()
    entries, i = [], 0
    while True:
        offset = struct.unpack_from('<I', blob, i)[0]
        i += 4
        if offset == 0:
            break
        name = bytearray()
        while blob[i] != 0:
            name.append(blob[i])
            i += 1
        i += 1
        entries.append((name.decode('latin1'), offset))

    for j, (name, offset) in enumerate(entries):
        end = entries[j + 1][1] if j + 1 < len(entries) else len(blob)
        yield name, blob[offset:end]


def read_chunks(emc):
    """Split the IFF-style EMC container into its TEXT/ORDR/DATA chunks."""
    chunks, i = {}, 12
    while i < len(emc) - 8:
        name = emc[i:i + 4].decode('latin1')
        length = struct.unpack_from('>I', emc, i + 4)[0]
        i += 8
        chunks[name] = emc[i:i + length]
        i += length + (length & 1)
    return chunks


def disassemble(data, functions, start, end):
    word = start
    while word < end and word * 2 < len(data):
        current = struct.unpack_from('>H', data, word * 2)[0]
        at, word = word, word + 1
        opcode, parameter = (current >> 8) & 0x1F, 0

        if current & 0x8000:
            opcode, parameter = 0, current & 0x7FFF
        elif current & 0x4000:
            parameter = struct.unpack('b', bytes([current & 0xFF]))[0]
        elif current & 0x2000:
            parameter = struct.unpack_from('>H', data, word * 2)[0]
            word += 1

        extra = ''
        if opcode == 14:
            extra = ' ' + functions[parameter & 0xFF]
        elif opcode == 17 and parameter < len(BINARY):
            extra = ' ' + BINARY[parameter]
        elif opcode == 15:
            parameter &= 0x7FFF

        print('%5d: %-13s %d%s' % (at, OPS.get(opcode, '?%d' % opcode), parameter, extra))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('pak', help='path to DUNE.PAK')
    parser.add_argument('script', nargs='?', default='UNIT.EMC')
    parser.add_argument('--list', action='store_true', help='list the archive contents')
    parser.add_argument('--type', type=int, help='disassemble one object type')
    parser.add_argument('--range', type=int, nargs=2, metavar=('FROM', 'TO'),
                        help='disassemble a word range')
    args = parser.parse_args()

    files = dict(read_pak(args.pak))
    if args.list:
        for name in files:
            print(name)
        return 0

    if args.script not in files:
        print('%s not found in %s' % (args.script, args.pak), file=sys.stderr)
        return 1

    chunks = read_chunks(files[args.script])
    data = chunks['DATA']
    order = chunks['ORDR']
    offsets = [struct.unpack_from('>H', order, j)[0] for j in range(0, len(order), 2)]
    functions = FUNCTIONS.get(args.script.upper(), UNIT_FN)

    if args.range:
        disassemble(data, functions, args.range[0], args.range[1])
    elif args.type is not None:
        start = offsets[args.type]
        following = sorted(o for o in offsets if o > start)
        disassemble(data, functions, start, following[0] if following else len(data) // 2)
    else:
        for index, offset in enumerate(offsets):
            print('type %2d -> word %d' % (index, offset))

    return 0


if __name__ == '__main__':
    sys.exit(main())
