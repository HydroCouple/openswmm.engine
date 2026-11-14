#!/usr/bin/env python3
"""Run the authorized A2 producer refinement and Phase 2 tolerance sweep."""

import argparse
import hashlib
import json
from pathlib import Path
import re

import numpy as np

from gw_clock_phase1 import relative_l2, run


def metrics(a, b):
    assert np.array_equal(a["time"], b["time"])
    return {
        field: {
            "max": float(np.max(np.abs(a[field] - b[field]))),
            "rms": float(np.sqrt(np.mean((a[field] - b[field]) ** 2))),
            "relative_l2": relative_l2(a[field], b[field])
            if np.linalg.norm(b[field])
            else None,
        }
        for field in ("head", "return")
    }


def report_stats(path):
    text = path.read_text(encoding="latin-1")
    stats = {}
    for key, pattern in {
        "surface_steps": r"Internal Steps\s*\.+\s*(\d+)",
        "surface_mean_s": r"Avg Internal Step \(s\)\s*\.+\s*([\d.]+)",
        "budget_percent": r"Budget-triggered firings \(%\)\s+([\d.eE+-]+)",
    }.items():
        found = re.search(pattern, text)
        if not found:
            raise ValueError(f"Missing {key}: {path}")
        stats[key] = float(found[1])
    stats["max_return_depth_m"] = max(
        map(float, re.findall(r"max return depth \(m\): ([\d.eE+-]+)", text))
    )
    return stats


def producer(cli, root):
    results, gates = {}, {}
    for case in ("coupled", "coupled_wide", "coupled_overflow"):
        for window in (1, 10, 30):
            rows, statistics = {}, {}
            for width in (0.125, 0.25, 0.5, 1.0):
                dest = root / f"{case}_{window}_step_{width}"
                dest.mkdir()
                rows[width] = run(
                    cli,
                    dest,
                    case,
                    "INDEPENDENT",
                    window,
                    0.35,
                    fixed_windows=True,
                    surface_max_step=width,
                )
                statistics[width] = report_stats(
                    dest / f"{case}_INDEPENDENT_{window}_0.35/model.rpt"
                )
                assert statistics[width]["surface_steps"] >= 3600 / width
                assert statistics[width]["surface_mean_s"] <= width + 5e-5
            errors = {
                width: metrics(rows[width], rows[0.125]) for width in (1.0, 0.5, 0.25)
            }
            for field in ("head", "return"):
                for norm in ("max", "rms"):
                    values = [errors[w][field][norm] for w in (1.0, 0.5, 0.25)]
                    # Equality below roundoff is resolved, not a failed convergence slope.
                    gates[f"{case}.{window}.{field}.{norm}"] = all(
                        fine <= coarse + 1e-12
                        for coarse, fine in zip(values, values[1:])
                    )
            results[f"{case}.{window}"] = {"errors": errors, "executed": statistics}
            print(case, window, errors, flush=True)
    return results, gates


def envelope(cli, root):
    rows, results, gates = {}, {}, {}
    for ceiling in (1, 10, 60, 600, 3600):
        dest = root / f"ceiling_{ceiling}"
        dest.mkdir()
        rows[ceiling] = run(
            cli,
            dest,
            "coupled_overflow",
            "INDEPENDENT",
            10,
            0.7,
            fixed_windows=True,
            gw_max_step=ceiling,
        )
        results[str(ceiling)] = {
            "errors": metrics(rows[ceiling], rows[1]),
            "executed": report_stats(
                dest / "coupled_overflow_INDEPENDENT_10_0.7/model.rpt"
            ),
        }
        print("ceiling", ceiling, results[str(ceiling)], flush=True)
    for field, limit in (("head", 0.01), ("return", 0.02)):
        errors = [
            results[str(c)]["errors"][field]["relative_l2"]
            for c in (1, 10, 60, 600, 3600)
        ]
        gates[f"{field}.default_accuracy"] = errors[-1] <= limit
        gates[f"{field}.temporal_convergence"] = all(
            fine <= coarse + 1e-12 for fine, coarse in zip(errors, errors[1:])
        )
    sweep = {}
    for beta in (0.1, 0.5, 0.9):
        for epsilon in (0.01, 0.05, 0.2):
            dest = root / f"budget_{beta}_{epsilon}"
            dest.mkdir()
            row = run(
                cli,
                dest,
                "coupled_overflow",
                "INDEPENDENT",
                10,
                0.7,
                fixed_windows=True,
                gw_options={"GW_BUDGET_FRACTION": beta, "GW_HEAD_TOLERANCE": epsilon},
            )
            sweep[f"{beta},{epsilon}"] = {
                "errors": metrics(row, rows[1]),
                "executed": report_stats(
                    dest / "coupled_overflow_INDEPENDENT_10_0.7/model.rpt"
                ),
            }
    results["tolerances"] = sweep
    return results, gates


def producer_checks(path, width):
    """Require every accepted producer endpoint, not just the reported mean."""
    if not path.exists():
        raise ValueError(
            "Controlled suite requires the passive controlled_timing_trace.patch build"
        )
    columns = np.loadtxt(path, delimiter=",", ndmin=2)
    ends = columns[:, 0]
    expected = np.arange(1, round(3600 / width) + 1, dtype=np.float64) * width
    np.testing.assert_array_equal(ends, expected)
    gaps = np.diff(ends, prepend=0.0)
    result = {
        "count": len(ends),
        "first_s": float(ends[0]),
        "last_s": float(ends[-1]),
        "min_step_s": float(gaps.min()),
        "max_step_s": float(gaps.max()),
        "sha256": hashlib.sha256(ends.astype("<f8").tobytes()).hexdigest(),
    }
    if columns.shape[1] > 1:
        assert np.all(columns[:, 1] == columns[0, 1])
        result["margin_s"] = float(columns[0, 1])
    return result


def controlled(cli, root):
    """Change only the GW ceiling within each verified producer-check sequence."""
    results, gates = {}, {}
    ceilings = (1, 10, 60, 600, 3600)
    for width in (1.0, 0.5, 0.25, 0.125):
        rows, runs = {}, {}
        for ceiling in ceilings:
            dest = root / f"step_{width}_ceiling_{ceiling}"
            dest.mkdir()
            rows[ceiling] = run(
                cli,
                dest,
                "coupled_overflow",
                "INDEPENDENT",
                10,
                0.7,
                fixed_windows=True,
                gw_max_step=ceiling,
                environment={"GW_CONTROL_STEP": str(width)},
            )
            folder = dest / "coupled_overflow_INDEPENDENT_10_0.7"
            checks = producer_checks(folder / "producer_checks.csv", width)
            if runs:
                assert checks["sha256"] == runs["1"]["producer_checks"]["sha256"]
            stats = report_stats(folder / "model.rpt")
            assert stats["surface_steps"] == checks["count"]
            surface_ceiling = float(
                re.search(r"MAX_TIMESTEP ([\d.]+)", (folder / "model.inp").read_text())[
                    1
                ]
            )
            runs[str(ceiling)] = {
                "errors": metrics(rows[ceiling], rows[1]),
                "executed": stats,
                "producer_checks": checks,
                "authored_surface_ceiling_s": surface_ceiling,
                "gw_check_margin_s": checks.get(
                    "margin_s", min(surface_ceiling, ceiling / 4)
                ),
            }
            print("controlled", width, ceiling, runs[str(ceiling)], flush=True)
        for field, limit in (("head", 0.01), ("return", 0.02)):
            errors = [runs[str(c)]["errors"][field]["relative_l2"] for c in ceilings]
            gates[f"{width}.{field}.default_accuracy"] = errors[-1] <= limit
            gates[f"{width}.{field}.temporal_convergence"] = all(
                fine <= coarse + 1e-12 for fine, coarse in zip(errors, errors[1:])
            )
        results[str(width)] = runs
    return results, gates


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--suite", choices=("producer", "envelope", "controlled"), required=True
    )
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(exist_ok=False)
    cli = args.engine.resolve()
    rows, gates = {
        "producer": producer,
        "envelope": envelope,
        "controlled": controlled,
    }[args.suite](cli, root)
    result = {
        "suite": args.suite,
        "cli_sha256": hashlib.sha256(cli.read_bytes()).hexdigest(),
        "results": rows,
        "gates": gates,
    }
    (root / "results.json").write_text(
        json.dumps(result, indent=2, allow_nan=False) + "\n"
    )
    if not all(gates.values()):
        raise SystemExit("Qualification failed; see results.json")


if __name__ == "__main__":
    main()
