#!/usr/bin/env python3
"""Independent rational audit of actual native receiver and remainder traces."""
import argparse
from fractions import Fraction
from pathlib import Path
import os
import subprocess
import math


def audit(path):
    lines = path.read_text().splitlines()
    assert lines and lines[0].startswith('initial '), path
    exact = Fraction(float.fromhex(lines[0].split()[1]))
    previous_remainder = Fraction(0)
    previous_mass = exact
    count = 0
    for line in lines[1:]:
        values = [Fraction(float.fromhex(x)) for x in line.split()]
        before, drain, spill, after = values[:4]
        remainder = sum(values[4:], Fraction(0))
        delta = drain - spill
        assert before == previous_mass, (path, count, 'native continuity')
        assert after + remainder == before + previous_remainder + delta, (path, count, 'local exact amount')
        assert after >= 0 and after + remainder >= 0, (path, count, 'negative inventory')
        exact += delta
        assert after + remainder == exact, (path, count, 'cumulative exact amount')
        reference = Fraction(float(before + delta))
        bound = Fraction(float(Fraction(1, 2**51) * abs(reference)))
        assert abs(before + delta - after) <= bound, (path, count, 'two-epsilon bound')
        if delta:
            assert after != before, (path, count, 'whole increment disappeared')
        else:
            assert after == before and remainder == previous_remainder, (path, count, 'zero-net correction')
        previous_mass, previous_remainder = after, remainder
        count += 1
    assert count, path
    return count


def audit_aggregates(path):
    lines = path.read_text().splitlines()
    exact = Fraction(float.fromhex(lines[0].split()[1]))
    previous_mass, previous_remainder = exact, Fraction(0)
    nonrepresentable = False
    for index, line in enumerate(lines[1:]):
        state, rows, parts = line.split('|')
        concentration, volume, following = [float.fromhex(v) for v in state.split()]
        amounts = [Fraction(float.fromhex(v)) for v in rows.split()]
        assert len(amounts) % 2 == 0
        drain, spill = sum(amounts[::2], Fraction(0)), sum(amounts[1::2], Fraction(0))
        delta = drain - spill
        nonrepresentable |= any(Fraction(float(v)) != v for v in [drain, spill, delta])
        before, after = Fraction(concentration * volume), Fraction(following * volume)
        remainder = sum((Fraction(float.fromhex(v)) for v in parts.split()), Fraction(0))
        assert before == previous_mass, (path, index, 'native continuity')
        expected_c = concentration if delta == 0 else (concentration * volume + float(delta)) / volume
        assert following == expected_c, (path, index, 'single correctly rounded mixer input')
        assert after + remainder == before + previous_remainder + delta, (path, index, 'exact aggregate receiving')
        exact += delta
        assert after + remainder == exact, (path, index, 'cumulative exact aggregate')
        assert after >= 0 and after + remainder >= 0
        reference = Fraction(float(before + delta))
        bound = Fraction(float(Fraction(1, 2**51) * abs(reference)))
        assert abs(before + delta - after) <= bound, (path, index, 'full exact target bound')
        if delta:
            assert float(delta) != 0 and after != before
        else:
            assert following == concentration and remainder == previous_remainder
        previous_mass, previous_remainder = after, remainder
    assert nonrepresentable, (path, 'did not exercise nonrepresentable aggregates')
    return len(lines) - 1


def audit_points(path):
    seen = set()
    multipart = 0
    for line in path.read_text().splitlines():
        identity, native, components = line.split('|')
        point, direction = map(int, identity.split())
        assert (point, direction) not in seen
        seen.add((point, direction))
        amounts = [Fraction(float.fromhex(v)) for v in native.split()]
        parts = [Fraction(float.fromhex(v)) for v in components.split()]
        assert len(amounts) == 2 and all(v >= 0 for v in amounts)
        exact = sum(amounts, Fraction(0))
        assert sum(parts, Fraction(0)) == exact, (path, point, 'exact point proof attribution')
        if Fraction(float(exact)) != exact:
            assert len(parts) > 1
            multipart += 1
    assert len(seen) == 6 and multipart > 0, (path, 'no authentic repeated multipart point')
    return len(seen)


def audit_history_points(path):
    seen = set()
    for line in path.read_text().splitlines():
        identity, native, components = line.split('|')
        key = tuple(map(int, identity.split()))
        assert len(key) == 3 and key not in seen
        seen.add(key)
        amounts = [Fraction(float.fromhex(v)) for v in native.split()]
        parts = [Fraction(float.fromhex(v)) for v in components.split()]
        assert len(amounts) == 1 and sum(parts, Fraction(0)) == sum(amounts, Fraction(0))
    assert len(seen) == 54 and len({key[0] for key in seen}) == 9
    return len(seen)


def audit_halves(path):
    lines = path.read_text().splitlines()
    previous_mass = Fraction(float.fromhex(lines[0].split()[1]))
    total, previous_r = previous_mass, Fraction(0)
    for index, line in enumerate(lines[1:]):
        state, raw, remainder = line.split('|')
        c, volume, following, span, dt, after_span, first = [float.fromhex(v) for v in state.split()]
        assert bool(first) == (index == 0) and span == (2*dt if first else dt) and after_span == max(0,span-dt)
        native = [float.fromhex(v) for v in raw.split()]
        slices = []
        for value in native:
            half = (value/(2*dt))*dt
            assert Fraction(half)*2 == Fraction(value) and value-half == half
            slices.append(Fraction(half))
        delta = sum(slices[::2], Fraction(0))-sum(slices[1::2], Fraction(0))
        before, after = Fraction(c*volume), Fraction(following*volume)
        r = sum((Fraction(float.fromhex(v)) for v in remainder.split()), Fraction(0))
        assert before == previous_mass and after+r == before+previous_r+delta
        assert following == (c*volume+float(delta))/volume
        total += delta
        assert after+r == total and after >= 0 and after+r >= 0
        reference = Fraction(float(before+delta))
        bound = Fraction(float(Fraction(1,2**51)*abs(reference)))
        assert abs(before+delta-after) <= bound and (delta == 0 or after != before)
        previous_mass, previous_r = after, r
    assert len(lines)==3
    return 2


def audit_half_points(path):
    seen = set()
    for line in path.read_text().splitlines():
        identity, raw, water, components = line.split('|')
        stage, point, direction = map(int,identity.split())
        assert (stage,point,direction) not in seen
        seen.add((stage,point,direction))
        legs = [Fraction(float.fromhex(v)) for v in raw.split()]
        assert len(legs)==4
        fraction = Fraction(1,2) if stage==1 else Fraction(0)
        assert Fraction(float.fromhex(water.strip())) == sum(legs[::2],Fraction(0))*fraction
        assert sum((Fraction(float.fromhex(v)) for v in components.split()),Fraction(0)) == sum(legs[1::2],Fraction(0))*fraction
    assert len(seen)==12
    return len(seen)


def audit_rounding(path):
    lines = path.read_text().splitlines()
    refused = 0
    for index, line in enumerate(lines):
        required, inputs, result = [x.strip() for x in line.split('|')]
        exact = sum((Fraction(float.fromhex(v)) for v in inputs.split()), Fraction(0))
        if result == 'refused':
            assert required == '0', (path, index, 'unnecessary rounding refusal')
            refused += 1
        else:
            value = float.fromhex(result)
            assert math.isfinite(value) and value == float(exact), (path, index, 'incorrect nearest/even rounding')
    assert len(lines) == 1040 and refused <= 2
    return len(lines)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--owner', type=Path, required=True)
    p.add_argument('--live', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, OPENSWMM_2D_BACKEND='cpu',
               OPENSWMM_PASSIVE_EXCHANGE_EVIDENCE=str(out), OPENSWMM_LIVE_EXCHANGE_EVIDENCE=str(out))
    for tag, binary, selector in [('owner', a.owner, 'PassiveStorageRoundoff.*:PassiveStorageAggregates.*:PassiveStoragePoints.*:PassiveStorageHalves.*'),
                                 ('live', a.live, 'LivePassiveRoundoff.*:LivePassiveAggregates.*:LivePassivePoints.*:LivePassiveHalves.*')]:
        with (out / (tag + '.log')).open('w') as log:
            subprocess.run([str(binary.resolve()), '--gtest_filter=' + selector,
                            '--gtest_output=xml:' + str(out / (tag + '.xml'))], env=env,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
    paths = [out / name for name in ['roundoff-positive.txt', 'roundoff-negative.txt', 'roundoff-partial.txt',
                                    'roundoff-live-positive.txt', 'roundoff-live-negative.txt']]
    count = sum(audit(path) for path in paths)
    count += sum(audit_aggregates(out / name) for name in ['aggregate-positive.txt', 'aggregate-negative.txt',
                 'aggregate-live-positive.txt', 'aggregate-live-negative.txt', 'point-live-positive.txt', 'point-live-negative.txt', 'point-history-positive.txt', 'point-history-negative.txt'])
    points = sum(audit_points(out / name) for name in ['point-live-positive-legs.txt', 'point-live-negative-legs.txt'])
    history_points = sum(audit_history_points(out / name) for name in ['point-history-positive-legs.txt', 'point-history-negative-legs.txt'])
    half_names = ['half-'+sign+flag for sign in ['positive','negative'] for flag in ['', '-preflight']]
    count += sum(audit_halves(out / (name+'.txt')) for name in half_names)
    half_points = sum(audit_half_points(out / (name+'-points.txt')) for name in half_names)
    rounded = audit_rounding(out / 'aggregate-rounding.txt')
    (out / 'exact-audit.txt').write_text(f'{count} native transitions satisfy exact receiver accounting; '
        f'{rounded} correctly rounded expansion cases; {points} exact repeated point totals; {history_points} exact history point totals; {half_points} exact half point records; physical global checks remain separate.\n')
    print((out / 'exact-audit.txt').read_text(), end='')


if __name__ == '__main__':
    main()
