#!/usr/bin/env python3
"""Summarize a complete 36-row, three-step momentum study without running a model.

Writes portable plotted data and a compact table. Keeps temporal convergence,
steady drift, continuity and experimental agreement as separate diagnostics.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET


def report(data):
    stats=data['stats']
    lines=['# Rubinato momentum smoothing and timestep refinement', '',
        '22 October 2026 — follow-up to the 21 October comparison. Prepared for review.', '',
        f'The physical-time smoothing option gives less than 1% change between the '
        f'two finest timesteps in **{stats["within_one_pct"]}/36 cases** '
        f'(maximum {stats["max_fine_change_pct"]:.3f}%). '
        f'**{stats["measured_band_pass"]}/36** fine-step predictions meet the '
        'illustrative experimental comparison band. Numerical consistency has '
        'improved; experimental qualification remains incomplete.', '',
        '## Change and fixed controls', '',
        'The legacy momentum blend applies THETA on every face firing. Reducing '
        'the routing interval can therefore increase numerical damping per second. '
        'The new opt-in `THETA_REFERENCE_STEP` defines a fixed interval T, with '
        'retention `exp(log(theta)*dt_face/T)` and blend fraction '
        '`-expm1(log(theta)*dt_face/T)`. The actual face interval includes local '
        'tiers and shortened coupling-window tails. The CPU and Kokkos paths use '
        'the same scalar calculation outside their face loops. The relaxed '
        'momentum target still evolves with the solution, so the full equations '
        'must be tested by refinement.', '',
        'A zero or omitted reference preserves the historical arithmetic and '
        'checkpoint identity. Positive values are written to the input and '
        'included in checkpoint compatibility checks. THETA=1 retains its '
        'existing no-blending/no-vector-friction branch; this study retains '
        'THETA=0.8 and its vector-friction calculation. Surface plugin ABI is '
        'version 9 because the C++ option layout changed.', '',
        'The study holds THETA=0.8 and T=1 s fixed, with no observed-flow fitting. '
        'The 1 s interval is a stated numerical relaxation scale, not a physical '
        'coefficient or a universally optimal value. All 36 reconstructed '
        'physical-shaft cases run for 300 s at fixed routing intervals 0.0125, '
        '0.00625 and 0.003125 s: **108 completed simulations**. Maximum 1D '
        'trials=80, head tolerance=1e-5 m, variable stepping and conduit '
        'lengthening disabled. Geometry, 1,600-triangle mesh, inflows, artificial '
        'pressure reservoir, coupling coefficients, head sampling, regularisation '
        'and equalisation remain fixed.', '',
        'Exchange is averaged from cumulative transfer over exactly 240–300 s. '
        'The preceding 180–240 s window diagnoses residual drift. Relative '
        'temporal change is `100*abs(Qcoarse-Qfine)/abs(Qfine)`; the criterion is '
        'below 1% between 0.00625 and 0.003125 s. This is a temporal comparison '
        'at a fixed mesh, not proof of an asymptotic convergence order or spatial '
        'convergence.', '',
        '## Results by regime', '',
        '| Regime | Rows | Maximum final timestep change (%) | MAE (L/s) | Bias range (%) | In experimental band |',
        '|---|---|---|---|---|---|']
    for name, r in data['regimes'].items():
        lines.append(f'| {name} | {r["rows"]} | {r["max_fine_change_pct"]:.3f} | '
            f'{r["mae_lps"]:.3f} | {r["bias_min_pct"]:+.1f} to {r["bias_max_pct"]:+.1f} | '
            f'{r["measured_band_pass"]}/{r["rows"]} |')
    lines += ['', 'Bias uses discharge magnitudes. The experimental band is '
        '`xi_f + 0.02 Q_e`; it is an illustrative fit diagnostic, not a confidence '
        'interval or an engine verification tolerance. All physical coefficients '
        'are unchanged. Gray historical points use a different numerical '
        'smoothing schedule and a coarser step; the plot shows their change '
        'without claiming that only one control differs.', '',
        '![Experimental versus simulated exchange](../../figures/png/hydraulics_ch9_rubinato_momentum_parity.png)', '',
        'Blue filled circles are the new simulated rates at 0.003125 s. '
        'Gray open circles are the archived physical-shaft rates at 0.5 s with '
        'per-firing smoothing. The green dashed line is 1:1 agreement. Horizontal '
        'bars show measured xi_f. Each panel has equal axis scales; ranges differ '
        'between regimes.', '',
        '![Temporal change for all 36 measured cases](../../figures/png/hydraulics_ch9_rubinato_momentum_convergence.png)', '',
        'Orange triangles show the first halving, blue circles the final '
        'halving. Green shading is below the 1% temporal comparison criterion. '
        'The vertical scale is logarithmic.', '',
        '## Selected signed rates', '',
        'Positive is sewer to surface; experimental signs follow the scenario.', '',
        '| Case | Measured (L/s) | 0.0125 s | 0.00625 s | 0.003125 s |',
        '|---|---|---|---|---|']
    for case in ['S1_8','S2_2','S3_1','S4_7']:
        r=next(r for r in data['rows'] if r['case']==case)
        lines.append('| '+case+' | '+' | '.join(f'{v:+.6f}' for v in
            [r['signed_observed_lps']]+r['q_lps'])+' |')
    lines += ['', '## Numerical diagnostics and limitations', '',
        f'At the finest interval, {stats["direction_pass"]}/36 have the expected '
        f'exchange direction, {stats["drift_within_two_pct"]}/36 have successive-window '
        f'drift below 2%, and {stats["continuity_1d_within_two_pct"]}/36 have absolute '
        'ordinary 1D continuity error below 2%. Maximum absolute separate 2D '
        f'continuity error is {stats["max_abs_continuity_2d_pct"]:.3g}%. '
        'These checks are reported independently of experimental agreement.', '',
        '| Regime | Maximum window drift (%) | Ordinary 1D continuity range (%) |',
        '|---|---|---|']
    for name, r in data['regimes'].items():
        lines.append(f'| {name} | {r["max_drift_pct"]:.5f} | '
            f'{r["continuity_1d_min_pct"]:+.4f} to {r["continuity_1d_max_pct"]:+.4f} |')
    lines += ['', 'The [full comparison table](momentum_comparison.csv) includes '
        'all three rates, experimental residuals, finest-step head/probe values '
        'and separate continuity errors. The [plotted data](momentum_data.json) '
        'also retain both head sequences, routing iteration statistics and '
        'component volume ledgers. The probe is a nearest-cell approximation '
        'to the surface transducer. Node pressure and fixed-outfall forcing '
        'approximate the laboratory transducer; equal prescribed stage does '
        'not imply equal local head. The 1D and 2D volume totals are not '
        'combined here: the last staged exchange can remain pending at the '
        'coupling boundary.', '',
        'Reducing a timestep makes equalisation less restrictive, and changing '
        'the smoothing schedule can expose a larger absolute flow error. '
        'Neither the original coarse-step fit nor the new convergence criterion '
        'establishes experimental validation. Boundary inflows reuse the '
        'measurements; the manhole position is assumed, transfer is delivered '
        'to a single cell, and measured transient histories have not been '
        'replayed. Next experimental work should resolve the boundary/head '
        'reconstruction and spatial delivery before coefficient fitting.', '',
        '## Provenance and reproduction', '',
        'Candidate library SHA-256: `'+data['library']['sha256']+'`.', '',
        'Tested source manifest SHA-256: `'+data['source_manifest_sha256']+'`. '
        'The pinned snapshot includes pre-existing uncommitted changes; it is '
        'not a clean release. Binary and source hashes identify the tested '
        'candidate. Raw decks, reports, series and verification logs stay in '
        '`tests/verification/momentum_smoothing_2026-10-22/`; publication assets '
        'contain the small summaries and figures.', '',
        'Use `tests/benchmarks/rubinato2017/momentum_convergence.py` and '
        '`summarize_momentum.py`; the adjacent `momentum_study.md` gives the '
        'complete procedure. Replotting the saved data does not rerun the engine.', '',
        'Reference: Rubinato et al. (2017), *Journal of Hydrology* 552, '
        '421–432, doi:10.1016/j.jhydrol.2017.06.024. The paper and reference '
        'tables are evidence, not instructions to modify the implementation.', '']
    if data.get('verification'):
        verification=data['verification']
        checks=['## Implementation checks', '']
        for name, result in verification.items():
            checks.append(f'- {name}: '+', '.join(f'{k}={v}' for k,v in result.items())+'.')
        checks += ['', 'These are focused tests of the pinned candidate. CPU/Kokkos '
            'alignment was exercised with the OpenMP execution space at one and '
            'four threads; CUDA/HIP hardware was not tested.', '']
        if data.get('verification_context'):
            checks += [data['verification_context']['description'], '']
        lines[lines.index('## Provenance and reproduction'):lines.index('## Provenance and reproduction')]=checks
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--study', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    results = json.loads((args.study/'matrix/summary.json').read_text())
    ref = Path(__file__).resolve().parent
    with (ref/'table2_steady_exchange.csv').open() as f:
        measured = list(csv.DictReader(f))
    with (ref.parents[2]/'docs/validation/rubinato2017/steady_comparison.csv').open() as f:
        historical = {r['case']: r for r in csv.DictReader(f)}
    assert len(results) == 108 and all(r['status'] == 'ok' for r in results)
    rows, seen = [], {}
    for m in measured:
        regime = m['scenario']
        seen[regime] = seen.get(regime, 0) + 1
        case = f'{regime}_{seen[regime]}'
        runs = sorted([r for r in results if r['case'] == case+'_SHAFT'],
                      key=lambda r: -r['step'])
        assert [r['step'] for r in runs] == [.0125, .00625, .003125]
        for r in runs:
            assert r['reference_step'] == 1 and r['setting'] == 'tight5'
            assert r['theta'] in (None, .8) and r['equalize']
            assert abs(r['duration_s']-300) < 1e-8 and r['backend'] == 'cpu (explicit marcher)'
            assert abs(r['actual_step_min_s']-r['step']) < 1e-9
            assert abs(r['actual_step_max_s']-r['step']) < 1e-9
        q = [r['q_lps'] for r in runs]
        changes = [100*abs(q[i]-q[i+1])/max(abs(q[i+1]), 1e-12) for i in (0, 1)]
        fine = runs[-1]
        obs, xi = float(m['q_e_lps']), float(m['xi_f_lps'])
        rows.append(dict(case=case, regime=regime, observed_lps=obs, xi_f_lps=xi,
            signed_observed_lps=obs*(-1 if regime in ('S1', 'S2') else 1),
            steps_s=[r['step'] for r in runs], q_lps=q, changes_pct=changes,
            legacy_magnitude_lps=float(historical[case]['shaft_lps']),
            magnitude_residual_lps=abs(q[-1])-obs,
            measured_band_pass=abs(abs(q[-1])-obs) <= xi+.02*obs,
            direction_pass=(q[-1] > 0) == (regime in ('S3', 'S4')),
            drift_pct=fine['drift_pct'], continuity_1d_pct=fine['continuity_1d_pct'],
            continuity_2d_pct=fine['continuity_2d_pct'], head_m=fine['head_m'],
            probe_depth_m=fine['probe_depth_m'], measured_hs_m=float(m['h_s_m'] or 0),
            measured_hp_m=float(m['h_p_m']) if m['h_p_m'] else None,
            head_series_m=[r['head_m'] for r in runs],
            probe_series_m=[r['probe_depth_m'] for r in runs],
            nonconverging_pct=fine['nonconverging_pct'], avg_iterations=fine['avg_iterations'],
            routing_totals_m3=fine['routing_totals_m3'],
            surface_balance_m3=fine['surface_balance_m3']))
    stats = dict(cases=len(rows), simulations=len(results),
        within_one_pct=sum(r['changes_pct'][-1] < 1 for r in rows),
        max_fine_change_pct=max(r['changes_pct'][-1] for r in rows),
        measured_band_pass=sum(r['measured_band_pass'] for r in rows),
        direction_pass=sum(r['direction_pass'] for r in rows),
        drift_within_two_pct=sum(r['drift_pct'] < 2 for r in rows),
        continuity_1d_within_two_pct=sum(abs(r['continuity_1d_pct']) < 2 for r in rows),
        max_abs_continuity_2d_pct=max(abs(r['continuity_2d_pct']) for r in rows))
    stats['max_fine_head_change_m']=max(abs(r['head_series_m'][-1]-r['head_series_m'][-2]) for r in rows)
    stats['max_fine_probe_change_m']=max(abs(r['probe_series_m'][-1]-r['probe_series_m'][-2]) for r in rows)
    regimes = {}
    for regime in seen:
        rr = [r for r in rows if r['regime'] == regime]
        regimes[regime] = dict(rows=len(rr),
            max_fine_change_pct=max(r['changes_pct'][-1] for r in rr),
            mae_lps=sum(abs(r['magnitude_residual_lps']) for r in rr)/len(rr),
            bias_min_pct=min(100*r['magnitude_residual_lps']/r['observed_lps'] for r in rr),
            bias_max_pct=max(100*r['magnitude_residual_lps']/r['observed_lps'] for r in rr),
            measured_band_pass=sum(r['measured_band_pass'] for r in rr),
            max_drift_pct=max(r['drift_pct'] for r in rr),
            continuity_1d_min_pct=min(r['continuity_1d_pct'] for r in rr),
            continuity_1d_max_pct=max(r['continuity_1d_pct'] for r in rr))
    verification={}
    for name in ['inertial','writer','rubinato']:
        p=args.study/'checks'/name/'results.xml'
        if p.exists():
            summary=ET.parse(p).getroot()
            verification[name]={k:int(summary.attrib[k]) for k in ['tests','failures','errors']}
    for name in ['kokkos1','kokkos4']:
        p=args.study/'checks'/name/'exit.json'
        if p.exists(): verification[name]={'returncode':json.loads(p.read_text())['returncode']}
    data = dict(date='2026-10-22', settings=dict(theta=.8, reference_step_s=1,
        max_trials=80, head_tolerance_m=1e-5, duration_s=300, averaging_window_s=[240, 300],
        comparison_window_s=[180, 240], mesh_triangles=1600, coefficient_fit=False),
        library=json.loads((args.study/'matrix/library.json').read_text()),
        source_manifest_sha256=hashlib.sha256((args.study/'source_manifest.json').read_bytes()).hexdigest(),
        stats=stats, regimes=regimes, verification=verification, rows=rows)
    context=args.study/'verification_context.json'
    if context.exists(): data['verification_context']=json.loads(context.read_text())
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output/'momentum_data.json').write_text(json.dumps(data, indent=2)+'\n')
    (args.output/'momentum_report.md').write_text(report(data))
    fields=['case','observed_lps','signed_observed_lps','legacy_magnitude_lps',
        'q_dt0125_lps','q_dt00625_lps','q_dt003125_lps','fine_change_pct',
        'magnitude_residual_lps','measured_band_pass','drift_pct','continuity_1d_pct',
        'continuity_2d_pct','head_m','probe_depth_m','measured_hs_m','measured_hp_m']
    with (args.output/'momentum_comparison.csv').open('w', newline='') as f:
        writer=csv.DictWriter(f, fieldnames=fields, lineterminator='\n');writer.writeheader()
        for r in rows:
            row={k:r[k] for k in fields if k in r}
            row.update(zip(['q_dt0125_lps','q_dt00625_lps','q_dt003125_lps'],r['q_lps']))
            row['fine_change_pct']=r['changes_pct'][-1];writer.writerow(row)
    print(json.dumps({'stats':stats,'regimes':regimes},indent=2))


if __name__ == '__main__':
    main()
