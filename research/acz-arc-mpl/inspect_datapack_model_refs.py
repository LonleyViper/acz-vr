"""Inspect repeated aircraft-path runs in an already-extracted datapack file.

This recognizes a narrowly observed 17-record, 36-byte path pattern. It does
not parse the container directory, establish how the game consumes it, or alter
the source. The model adjacent to an aircraft's assets is a static lead only.
"""
import argparse
import hashlib
import json
from pathlib import Path

RECORD_SIZE = 36
RECORD_COUNT = 17
SUFFIX = b'\x10\x00\x00\x00'


def path_record(data, offset):
    record = data[offset:offset + RECORD_SIZE]
    if len(record) != RECORD_SIZE or record[32:] != SUFFIX:
        return None
    field = record[:32]
    name, sep, padding = field.partition(b'\0')
    if not sep or any(padding) or not name or any(b < 32 or b > 126 for b in name):
        return None
    return name.decode('ascii')


def inspect(data, aircraft):
    if not aircraft or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789' for c in aircraft):
        raise ValueError('aircraft id must be lowercase alphanumeric')
    marker = ('mis/mpl/' + aircraft + '.pmb').encode('ascii')
    matches, rejected = [], []
    at = 0
    while True:
        at = data.find(marker, at)
        if at < 0:
            break
        records = [path_record(data, at + i * RECORD_SIZE) for i in range(RECORD_COUNT)]
        if (all(records) and records[0] == marker.decode('ascii')
                and records[1].startswith('plmodel/')
                and records[2] == 'mis/mpl/' + aircraft + '.pvb'):
            matches.append({'offset': at, 'model_path': records[1], 'paths': records})
        else:
            rejected.append(at)
        at += len(marker)
    return {'aircraft_id': aircraft, 'matches': matches, 'rejected_offsets': rejected}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('datapack', type=Path)
    parser.add_argument('--aircraft', action='append', required=True)
    parser.add_argument('--out', type=Path, required=True, help='new JSON file')
    args = parser.parse_args()
    try:
        data = args.datapack.read_bytes()
        result = {'schema': 'acz-datapack-model-reference-survey/v1',
                  'source_sha256': hashlib.sha256(data).hexdigest(), 'source_bytes': len(data),
                  'record_size': RECORD_SIZE, 'records_per_candidate': RECORD_COUNT,
                  'aircraft': [inspect(data, aircraft) for aircraft in args.aircraft],
                  'limitations': ['Repeated adjacent path records are static evidence only.',
                                  'Container indexing, variant meaning and runtime selection are not established.',
                                  'An adjacent model reference does not prove aircraft identity or camera alignment.']}
        with args.out.open('x', encoding='utf-8', newline='\n') as output:
            json.dump(result, output, indent=2)
            output.write('\n')
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print('Surveyed %d aircraft ids' % len(result['aircraft']))


if __name__ == '__main__':
    main()
