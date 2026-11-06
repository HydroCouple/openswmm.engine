#!/usr/bin/env python3
"""Archive the dated comparison and actual T9 surface frames for figure rebuilds.

Requires numpy and h5py. Reads saved output only; never runs or tunes a model.
The resulting JSON is the portable input to ch9_rubinato_comparison.py.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path

import h5py
import numpy as np

ENGINE = Path(__file__).resolve().parents[3]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--model-run', type=Path, required=True)
    ap.add_argument('--output', type=Path, default=ENGINE/'docs/figures/data/rubinato2017.json')
    args = ap.parse_args()
    review = ENGINE/'docs/validation/rubinato2017'
    def read_csv(path):
        with path.open(newline='') as f:
            return list(csv.DictReader(f))
    steady = read_csv(review/'steady_comparison.csv')
    for r in steady:
        for k, v in r.items():
            if k != 'case':
                r[k] = float(v) if v else None
    summary = json.loads((review/'results_summary.json').read_text())
    metrics = read_csv(args.model_run/'metrics.csv')
    times = np.array([float(r['time_s']) for r in metrics])
    net = np.array([float(r['net_to_surface_m3']) for r in metrics])
    q = np.array([float(r['q_to_surface_lps']) for r in metrics])
    increments = np.diff(np.r_[0, net])
    spill = np.cumsum(np.maximum(increments, 0))
    drain = np.cumsum(np.maximum(-increments, 0))
    totals = json.loads((args.model_run/'metrics.json').read_text())
    assert np.isclose(spill[-1], totals['spill_m3'], atol=1e-9)
    assert np.isclose(drain[-1], totals['drain_m3'], atol=1e-9)
    with h5py.File(args.model_run/'model.2d.h5') as f:
        # Output uses SWMM date-days and carries a ~1 ms reporting offset.
        # Subtract midnight, retaining the recorded offset (not a fabricated t=0).
        frame_times = (f['time'][:]-np.floor(f['time'][0]))*86400
        depths = f['Mesh2_face_depth'][:]*1000
        triangles = f['Mesh2_face_nodes'][:].astype(int)
        start = int(f['Mesh2_face_nodes'].attrs['start_index'])
        triangles -= start
        nodes = np.c_[f['Mesh2_node_x'][:], f['Mesh2_node_y'][:]]
        areas = f['Mesh2_face_area'][:]
    assert depths.shape == (60, 1600) and nodes.shape == (861, 2)
    assert len(steady) == 36 and np.all(depths >= 0) and np.isclose(areas.sum(), 32)
    assert triangles.min() == 0 and triangles.max() < len(nodes)
    sources = [review/'steady_comparison.csv', review/'results_summary.json'] + [
        args.model_run/n for n in ['model.inp', 'model.2d.h5', 'metrics.csv', 'metrics.json']]
    provenance = dict(date='2026-10-21', measured_source='Rubinato et al. (2017), Table 2; CC-BY-4.0',
        doi='10.1016/j.jhydrol.2017.06.024', engine_sha=summary['engine_sha'],
        binary_manifest=summary['binary_manifest'],
        source_sha256={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
        note='Steady: magnitudes at 0.5 s, not converged. T9: synthetic forcing, no measured transient curve.',
        frame_note='60 saved cell-depth frames, ~10.001 through 600.001 s; no invented initial frame; depths rounded to 0.001 mm.')
    data = dict(provenance=provenance, steady=steady, refinement=summary['routing_refinement'],
        transient=dict(times_s=np.round(times, 6).tolist(), q_lps=np.round(q, 6).tolist(),
            spill_m3=np.round(spill, 8).tolist(), drain_m3=np.round(drain, 8).tolist(),
            frame_times_s=np.round(frame_times, 6).tolist(), nodes=np.round(nodes, 6).tolist(),
            triangles=triangles.tolist(), depths_mm=np.round(depths.astype(np.float64), 3).tolist(),
            totals=totals, saved_frame_max_mm=float(depths.max())))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(data, separators=(',', ':'), allow_nan=False)+'\n')
    print(f'Archived {len(steady)} measured rows, {len(frame_times)} actual frames, {len(times)} exchange samples: {args.output}')


if __name__ == '__main__':
    main()
