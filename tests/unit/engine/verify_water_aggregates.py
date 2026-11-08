#!/usr/bin/env python3
"""Independent Fraction audit of native directional water aggregation and delivery."""
import argparse
from collections import defaultdict
from fractions import Fraction as F
from pathlib import Path
import json
import math
import os
import struct
import subprocess
import zlib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--engine', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
native = out / 'native'
env = dict(os.environ, OPENSWMM_2D_BACKEND='cpu', OPENSWMM_CHANGING_VOLUME_EVIDENCE=str(native))
with (out / 'authored.log').open('w') as log:
    subprocess.run([str(args.engine.resolve()), '--gtest_filter=ChangingVolumeWaterAggregates.Authored*',
                    '--gtest_output=xml:' + str(out / 'authored.xml')],
                   env=env, stdout=log, stderr=subprocess.STDOUT, check=True)

def values(text):
    return [float.fromhex(v) for v in text.split()]

def exact(scalar, parts):
    assert math.isfinite(scalar) and scalar >= 0
    if not parts:
        return F(scalar)
    assert scalar == 0 and 2 <= len(parts) <= 64 and all(math.isfinite(v) and v != 0 for v in parts)
    total = sum(map(F, parts), F(0))
    assert total > 0
    return total

class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def get(self, fmt):
        fmt = '<' + fmt
        result = struct.unpack_from(fmt, self.data, self.at)
        self.at += struct.calcsize(fmt)
        return result[0] if len(result) == 1 else result

    def blob(self):
        count = self.get('Q')
        result = self.data[self.at:self.at + count]
        assert len(result) == count
        self.at += count
        return result

    def vector(self):
        return [self.get('d') for _ in range(self.get('Q'))]

    def strings(self):
        return [self.blob() for _ in range(self.get('Q'))]

    def finish(self):
        assert self.at == len(self.data)

def envelope(data, magic, version):
    r = Reader(data)
    assert r.blob() == magic.encode()
    assert r.get('II') == (version, 0x01020304)
    checksum = r.get('I')
    payload = r.blob()
    assert zlib.crc32(payload) == checksum
    r.finish()
    return Reader(payload)

def water_publications(path):
    """Decode actual saved queue observations independently of the engine reader."""
    r = envelope(path.read_bytes(), 'OpenSWMM live passive owner', 10)
    header = r.get('dQQQddBBQddddQ')
    for _ in range(header[-1]):
        r.get('ii')
    b = envelope(r.blob(), 'OpenSWMM changing-volume storage receipt owner', 8)
    r.finish()
    b.strings(); b.strings(); b.get('bQddddddd')
    nodes = b.get('Q')
    assert nodes == 1
    for _ in range(nodes):
        b.get('iddd')
    for _ in range(b.get('Q')):
        b.get('QBdddddQ')
        for _ in range(b.get('Q')):
            for _ in range(2):
                b.get('Qi'); b.blob(); b.get('bdddd')
                for _ in range(4):
                    b.vector()
            b.get('ddd')
        assert b.get('B') == 1
        b.get('d')
        for _ in range(b.get('Q')):
            b.get('Qiid')
        for _ in range(b.get('Q')):
            b.get('QiiBB'); b.get('dddddddddBddddd')
        b.get('B')
    for _ in range(b.get('Q')):
        b.get('ddddQBd')
        for _ in range(33):
            assert len(b.vector()) == nodes
    result = {}
    for _ in range(b.get('Q')):
        b.get('BdddddQ')
        receipt = b.get('Q')
        dt, remaining, factor = b.get('ddd')
        points = [b.get('Qiid') for _ in range(b.get('Q'))]
        before, after = b.vector(), b.vector()
        assert len(before) == len(after) == nodes and b.get('Q') == nodes
        discrepancy = b.vector()
        assert receipt not in result
        result[receipt] = dict(points=points, factor=factor, before=before[0], after=after[0],
                               discrepancy=sum(map(F, discrepancy), F(0)))
    for _ in range(b.get('Q')):
        b.get('iddd')
    for _ in range(b.get('Q')):
        b.get('i'); b.vector()
    b.finish()
    return result

records, booking = [], []
quality_steps = multipart = half_rows = 0
directions = set()
for mode in ['whole', 'half']:
    for sign in ['positive', 'negative']:
        tag = f'water-aggregate-{mode}-{sign}'
        legs = defaultdict(list)
        for line in (native / (tag + '-legs.txt')).read_text().splitlines():
            columns = line.split()
            step, receipt, point, drain = map(int, columns[:4])
            water, mass, si = values(' '.join(columns[4:]))
            legs[step, receipt, point, drain].append((water, mass, si))
        rows = {}
        for line in (native / (tag + '-rows.txt')).read_text().splitlines():
            identity, wp, mp = line.split('|')
            columns = identity.split()
            step, receipt, remaining, point, drain = map(int, columns[:5])
            water, mass = values(' '.join(columns[5:]))
            water_parts, mass_parts = values(wp), values(mp)
            key = step, receipt, remaining, point, drain
            assert key not in rows
            rows[key] = exact(water, water_parts), exact(mass, mass_parts)
            if water_parts:
                multipart += 1
                directions.add(drain)
        consumed, pending_spill = defaultdict(F), defaultdict(F)
        case_multipart = case_halves = 0
        for key, (water, mass) in rows.items():
            step, receipt, remaining, point, drain = key
            if remaining:
                continue
            raw = legs.get((step, receipt, point, drain), [])
            expected_water = sum((F(v[0]) for v in raw), F(0))
            expected_mass = sum((F(v[1]) for v in raw), F(0))
            assert water == expected_water and mass == expected_mass, (tag, key, 'gross provenance')
            remainder_water, remainder_mass = rows[step, receipt, 1, point, drain]
            if water:
                assert len(raw) == 4, (tag, key, 'native firings')
                fraction = remainder_water / water
                assert fraction in ([F(0), F(1)] if mode == 'whole' else [F(0), F(1, 2), F(1)])
                assert remainder_mass == mass * fraction, (tag, key, 'water/material partition')
                if fraction == F(1, 2):
                    case_halves += 1
                    for w, m, _ in raw:
                        assert F((w / (1 / 512)) * (1 / 1024)) == F(w) / 2
                        assert F((m / (1 / 512)) * (1 / 1024)) == F(m) / 2
                if F(float(water)) != water:
                    case_multipart += 1
            else:
                assert mass == remainder_water == remainder_mass == 0
            consumed[step] += (mass - remainder_mass) * (1 if drain else -1)
            if not drain:
                pending_spill[step] += remainder_mass
            records.append(dict(case=tag, step=step, receipt=receipt, point=point, drain=drain,
                                water=str(water), water_remaining=str(remainder_water), mass=str(mass),
                                mass_remaining=str(remainder_mass)))
        assert case_multipart > 0 and (case_halves > 0 if mode == 'half' else case_halves == 0), tag
        half_rows += case_halves
        point_groups = defaultdict(list)
        for line in (native / (tag + '-points.txt')).read_text().splitlines():
            columns = line.split(); step, receipt, point = map(int, columns[:3])
            si, factor = values(' '.join(columns[3:]))
            # Native point order is chronological. Each authored point has one direction.
            raw = [(drain, v) for drain in [1, 0] for v in legs.get((step, receipt, point, drain), [])]
            assert not (legs.get((step, receipt, point, 1)) and legs.get((step, receipt, point, 0)))
            actual_si = 0.0
            gross = F(0)
            for drain, (water, mass, leg_si) in raw:
                actual_si += leg_si if drain else -leg_si
                assert water == leg_si * factor
                gross += F(water) * (1 if drain else -1)
            assert si == actual_si, (tag, step, receipt, point, 'native signed SI accumulation')
            point_groups[step, receipt].append((si * factor, gross))
        for (step, receipt), points in point_groups.items():
            booked = 0.0
            for value, _ in points:
                booked += value
            gross = sum((v[1] for v in points), F(0))
            observed = water_publications(native / f'{tag}-{step}.owner')[receipt]
            assert [v[3] * observed['factor'] for v in observed['points']] == [v[0] for v in points]
            assert observed['discrepancy'] == F(booked) - gross
            assert observed['after'] == observed['before'] + booked
            booking.append(dict(case=tag, step=step, receipt=receipt, native_booking=booked.hex(),
                                exact_gross=str(gross), diagnostic_difference=str(F(booked) - gross),
                                queue_before=observed['before'].hex(), queue_after=observed['after'].hex(),
                                queue_addition_roundoff=str(F(observed['after']) - F(observed['before']) - F(booked))))
        previous_mass = previous_consumed = initial = None
        for line in (native / (tag + '-states.txt')).read_text().splitlines():
            columns = line.split(); step = int(columns[0])
            volume, old_volume, concentration, old_c = values(' '.join(columns[1:5]))
            remainder = sum(map(F, values(' '.join(columns[5:]))), F(0))
            mass = F(concentration * volume); delivered = consumed[step]
            if step == 0:
                initial = mass
            else:
                delta = delivered - previous_consumed
                assert F(old_c * old_volume) == previous_mass
                target = previous_mass + delta
                assert concentration == float(F(float(previous_mass + F(float(delta)))) / F(volume))
                assert abs(target - mass) <= F((2 * 2**-52) * abs(float(target)))
                if delta:
                    baseline = float(previous_mass / F(volume))
                    assert concentration != baseline and mass != F(baseline * volume)
                quality_steps += 1
            assert mass + remainder == initial + delivered, (tag, step, 'exact accounted inventory')
            assert mass >= pending_spill[step] and mass + remainder >= pending_spill[step]
            previous_mass, previous_consumed = mass, delivered
assert quality_steps == 16 and multipart > 0 and directions == {0, 1} and half_rows > 0
assert any(F(v['diagnostic_difference']) != 0 for v in booking)
(out / 'accounting.json').write_text(json.dumps(dict(quality_steps=quality_steps, multipart_rows=multipart,
    exact_half_rows=half_rows, directional_records=records, native_booking_records=booking), indent=2) + '\n')
print(f'{quality_steps} native receiver transitions, {multipart} multipart water rows and {half_rows} exact-half rows pass Fraction accounting.')
