#!/usr/bin/env python3
"""Phase 2 stale-head node saturation: compare actual per-window transfers."""

import argparse
import json
from pathlib import Path
import re

import h5py
import numpy as np

from gw_clock_accuracy import ROOT, run
from soa_review_baseline import engine, sha


def compare_packets(candidate, reference):
    delta = candidate - reference
    relative_l2 = float(np.linalg.norm(delta) / np.linalg.norm(reference))
    event_error = float(abs(candidate.sum() - reference.sum()) / abs(reference.sum()))
    return {
        "per_window_relative_l2": relative_l2,
        "event_volume_relative_error": event_error,
        "max_window_error_m3": float(np.max(np.abs(delta))),
    }, {
        "per_window_l2_2pct": relative_l2 <= .02,
        "each_window_2pct": bool(np.all(np.abs(delta) <= .02 * np.abs(reference))),
        "event_total_2pct": event_error <= .02,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--reference-engine", type=Path,
        help="explicit matched-physics SHARED reference; production SHARED is retained as compatibility evidence",
    )
    parser.add_argument(
        "--gw-base-step",
        type=float,
        default=1,
        help="authored groundwater base cadence in seconds",
    )
    args = parser.parse_args()
    if not np.isfinite(args.gw_base_step) or args.gw_base_step <= 0:
        parser.error("--gw-base-step must be finite and positive")
    cli = args.engine.resolve()
    reference_cli = args.reference_engine.resolve() if args.reference_engine else cli
    reference = "REFERENCE" if args.reference_engine else "SHARED"
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    source = ROOT / "tests/benchmarks/generated/gw2d_clock/node_saturation.inp"
    result = {
        "engine": engine(cli),
        "reference": {"kind": "matched-physics SHARED" if args.reference_engine else "legacy SHARED",
                      "engine": engine(reference_cli)},
        "script_sha256": sha(Path(__file__)),
        "source": str(source),
        "source_sha256": sha(source),
        "gw_base_step": args.gw_base_step,
        "saturation_guard_fraction": 1e-4,  # Native kSatGuard in SubsurfaceSolver.cpp.
        "windows": {},
        "gates": {},
        "legacy_compatibility": {"windows": {}, "gates": {}},
    }
    for window in (1, 10, 30):
        folder = output / str(window)
        folder.mkdir()
        text = re.sub(
            r"^ROUTING_STEP .*$",
            f"ROUTING_STEP {window}",
            source.read_text(),
            flags=re.M,
        )
        text = re.sub(
            r"^REPORT_STEP .*$", f"REPORT_STEP 00:00:{window:02d}", text, flags=re.M
        )
        authored = folder / "source.inp"
        authored.write_text(text)
        rows, packets, stats = {}, {}, {}
        runs = [("SHARED", cli, "SHARED"), ("INDEPENDENT", cli, "INDEPENDENT")]
        if args.reference_engine:
            runs.append(("REFERENCE", reference_cli, "SHARED"))
        for mode, binary, clock_mode in runs:
            rows[mode], stats[mode] = run(
                binary, folder / mode, authored, mode=clock_mode, base_step=args.gw_base_step
            )
            with h5py.File(folder / mode / "fields.2d.h5") as h:
                # Positive into the cell; each report is one actual hydraulic
                # window. Do not infer this from instantaneous flow samples.
                cumulative = -h["groundwater_node_exchange_cum"][:, 0]
                packets[mode] = np.diff(cumulative, prepend=0.0)
            log = (folder / mode / "console.log").read_text()
            count = re.search(r"(\d+) steps completed", log)
            assert count and int(count[1]) == 120 // window
        np.testing.assert_array_equal(
            rows[reference]["time"], rows["INDEPENDENT"]["time"]
        )
        metrics, packet_gates = compare_packets(packets["INDEPENDENT"], packets[reference])
        legacy_metrics, legacy_gates = compare_packets(packets["INDEPENDENT"], packets["SHARED"])
        result["legacy_compatibility"]["windows"][str(window)] = legacy_metrics
        result["legacy_compatibility"]["gates"].update(
            {f"{window}.{key}": passed for key, passed in legacy_gates.items()}
        )
        result["windows"][str(window)] = {
            **metrics,
            "packets_m3": {mode: p.tolist() for mode, p in packets.items()},
            "max_head_m": {
                mode: float(row["head"].max()) for mode, row in rows.items()
            },
            "executed": stats,
        }
        result["gates"].update(
            {
                **{f"{window}.{key}": passed for key, passed in packet_gates.items()},
                f"{window}.budget_pulls_forward": stats["INDEPENDENT"]["budget_percent"]
                > 0,
                f"{window}.both_reach_native_saturation_guard": all(
                    rows[mode]["head"].max() >= 1 - result["saturation_guard_fraction"]
                    for mode in ("INDEPENDENT", reference)
                ),
                f"{window}.both_stop_receiving": all(
                    packets[mode][-1] == 0 for mode in ("INDEPENDENT", reference)
                ),
            }
        )
        (output / "results.json").write_text(
            json.dumps(result, indent=2, allow_nan=False) + "\n"
        )
        print(
            window, reference, metrics, flush=True
        )
    print("failed gates:", [k for k, value in result["gates"].items() if not value])
    return 0 if all(result["gates"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
