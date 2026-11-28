#!/usr/bin/env python3
"""Independent exact accounting of observed changing-volume native receiver steps."""
from fractions import Fraction as F
from pathlib import Path
import argparse
import json
import os
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--engine', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
args = p.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, OPENSWMM_2D_BACKEND='cpu', OPENSWMM_CHANGING_VOLUME_EVIDENCE=str(out / 'native'))
with (out / 'authored.log').open('w') as log:
    subprocess.run([str(args.engine.resolve()), '--gtest_filter=ChangingVolumeExchange.Authored*',
                    '--gtest_output=xml:' + str(out / 'authored.xml')], env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
records = []
tails = 0
for tag in ['flow-positive', 'flow-negative', 'tail-positive', 'tail-negative']:
    consumed, pending_spill = {}, {}
    for line in (out / 'native' / (tag + '-amounts.txt')).read_text().splitlines():
        step, remaining, drain, part = line.split()
        step, remaining, drain, value = int(step), int(remaining), int(drain), F(float.fromhex(part))
        consumed[step] = consumed.get(step, F(0)) + value * (1 if drain else -1) * (-1 if remaining else 1)
        if remaining and not drain:
            pending_spill[step] = pending_spill.get(step, F(0)) + value
    previous_mass = previous_consumed = initial = None
    for line in (out / 'native' / (tag + '-material.txt')).read_text().splitlines()[1:]:
        columns = line.split()
        step = int(columns[0])
        volume, old_volume, concentration, old_c, pd, ps = map(float.fromhex, columns[1:7])
        remainder = sum((F(float.fromhex(v)) for v in columns[7:]), F(0))
        mass = F(concentration * volume)
        delivered = consumed.get(step, F(0))
        if step == 0:
            initial = mass
        else:
            delta = delivered - previous_consumed
            assert F(old_c * old_volume) == previous_mass
            rounded_input = float(delta)
            numerator = float(previous_mass + F(rounded_input))
            assert concentration == float(F(numerator) / F(volume)), (tag, step, 'mixing law')
            target = previous_mass + delta
            error = target - mass
            bound = F((2 * 2**-52) * abs(float(target)))
            assert abs(error) <= bound, (tag, step, 'local roundoff')
            if delta:
                baseline = float(previous_mass / F(volume))
                assert concentration != baseline and mass != F(baseline * volume)
            if tag.startswith('tail-') and step == 5:
                assert delta == 0 and volume != old_volume and concentration != old_c
                tails += 1
            records.append(dict(case=tag, step=step, delivered=str(delta), physical=str(mass), remainder=str(remainder)))
        assert mass + remainder == initial + delivered, (tag, step, 'exact inventory')
        assert mass >= pending_spill.get(step, F(0)) and mass + remainder >= pending_spill.get(step, F(0))
        previous_mass, previous_consumed = mass, delivered
assert len(records) == 28 and tails == 2
(out / 'accounting.json').write_text(json.dumps(dict(steps=28, zero_material_hydraulic_tails=tails, records=records), indent=2) + '\n')
print('28 native receiver steps preserve exact accounted inventory; both zero-material hydraulic tails pass.')
