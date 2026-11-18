#!/usr/bin/env python3
"""Run the D-10 water-only bed intervention with the isolated instrumented binary."""

import argparse
import hashlib
import json
import os
from pathlib import Path

import h5py
import numpy as np

import gw_clock_phase1 as phase1

WINDOWS = (1, 10, 30)
MODES = ("SHARED", "INDEPENDENT")


def summary(journal):
    packets = np.loadtxt(journal, delimiter=",", skiprows=1, ndmin=2)
    assert packets.shape[1] == 3 and np.isfinite(packets).all()
    assert np.all(np.diff(packets[:, 0]) > 0) and np.all(packets[:, 1] > 0)
    assert np.all(packets[:, 0] <= packets[:, 2])
    # Hash just the physical payload. The receiving surface check is measured separately.
    payload = np.ascontiguousarray(packets[:, :2], dtype="<f8")
    result = {
        "packets": len(packets),
        "volume_m3": float(np.sum(packets[:, 1])),
        "payload_sha256": hashlib.sha256(payload.tobytes()).hexdigest(),
        "max_delivery_lag_s": float(np.max(packets[:, 2] - packets[:, 0])),
    }
    trace = journal.with_name("early.csv")
    if trace.exists():
        with trace.open() as stream:
            next(stream)
            added = coincident = 0
            for line in stream:
                if line.rstrip().endswith(",0"):
                    added += 1
                else:
                    coincident += 1
        result.update(additional_early=added, already_due=coincident)
    return result


def compare_control(data, path):
    with h5py.File(path / "fields.2d.h5") as f:
        for field, dataset in (
            ("time", "time"),
            ("head", "Mesh2_face_gw_hg"),
            ("return", "Mesh2_face_gw_dunne_cum"),
        ):
            np.testing.assert_array_equal(data[field], f[dataset][:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--production-reference", type=Path, required=True)
    journal_source = parser.add_mutually_exclusive_group()
    journal_source.add_argument(
        "--replay-from",
        type=Path,
        help="Use this existing donor journal across the interventions",
    )
    parser.add_argument(
        "--capacity-clock",
        action="store_true",
        help="Refresh independent aquifer capacity at the first surface check crossing INFIL_STEP",
    )
    journal_source.add_argument(
        "--events-from",
        type=Path,
        help="Land the surface at each delivery time in this recorded journal",
    )
    parser.add_argument(
        "--case", choices=("coupled", "coupled_wide"), default="coupled"
    )
    parser.add_argument("--cfl", type=float, choices=(0.35, 0.7), default=0.35)
    args = parser.parse_args()
    if any(key.startswith("GW_STUDY_") for key in os.environ):
        parser.error(
            "Clear inherited GW_STUDY_ variables so each scenario selects its own interventions."
        )
    cli, dest = args.engine.resolve(), args.output.resolve()
    dest.mkdir(parents=True, exist_ok=False)
    case, cfl = args.case, args.cfl
    results = {"case": case, "cfl": cfl, "scenarios": {}}
    production = json.loads((args.production_reference / "results.json").read_text())
    fixed_windows = production.get("fixed_windows", False)
    results["fixed_windows"] = fixed_windows
    production_floor = production[f"{case}.{cfl}.SHARED"]
    results["production_shared_spread"] = production_floor
    reference = dest / "live" / f"{case}_INDEPENDENT_1_{cfl}" / "bed.csv"
    if args.events_from:
        reference = args.events_from.resolve()
    if args.replay_from:
        reference = args.replay_from.resolve()
    results["exact_delivery_events"] = bool(args.events_from)
    results["capacity_clock"] = args.capacity_clock
    for scenario, fixed, policy in (
        ("live", False, None),
        ("fixed", True, None),
        ("live_budget", False, "budget"),
        ("fixed_budget", True, "budget"),
        ("live_any", False, "any"),
        ("fixed_any", True, "any"),
    ):
        stage = dest / scenario
        stage.mkdir()
        env = {"GW_STUDY_AUDIT": "1"}
        if args.events_from:
            env["GW_STUDY_EVENTS"] = str(args.events_from.resolve())
        if args.capacity_clock:
            env["GW_STUDY_CAPACITY"] = "1"
        if fixed:
            env["GW_STUDY_REPLAY"] = str(reference)
            reference_hash = summary(reference)["payload_sha256"]
        if policy:
            env["GW_STUDY_EARLY"] = policy
        for mode in ("INDEPENDENT",) if policy else MODES:
            rows = []
            journals = {}
            for window in WINDOWS:
                data = phase1.run(
                    cli,
                    stage,
                    case,
                    mode,
                    window,
                    cfl,
                    environment=env,
                    fixed_windows=fixed_windows,
                )
                name = f"{case}_{mode}_{window}_{cfl}"
                journal = summary(stage / name / "bed.csv")
                if (
                    scenario == "live"
                    and not args.events_from
                    and (not args.capacity_clock or mode == "SHARED")
                ):
                    compare_control(data, args.production_reference / name)
                if fixed:
                    assert journal["payload_sha256"] == reference_hash
                    if args.events_from:
                        assert journal["max_delivery_lag_s"] <= 1e-12
                if (
                    scenario == "fixed"
                    and not args.events_from
                    and not args.capacity_clock
                    and not args.replay_from
                    and mode == "INDEPENDENT"
                    and window == 1
                ):
                    compare_control(data, args.production_reference / name)
                rows.append(data)
                journals[window] = journal
            spreads = phase1.measure(rows)
            result = dict(spreads=spreads, journals=journals)
            if mode == "INDEPENDENT":
                floors = {
                    "production": production_floor,
                    "live": results["scenarios"]["live.SHARED"]["spreads"],
                }
                if fixed:
                    floors["fixed"] = results["scenarios"]["fixed.SHARED"]["spreads"]
                for floor, shared in floors.items():
                    result[f"within_2x_{floor}_shared"] = {
                        field: {
                            stat: values[stat] <= 2 * shared[field][stat]
                            for stat in values
                        }
                        for field, values in spreads.items()
                    }
            results["scenarios"][f"{scenario}.{mode}"] = result
            (dest / "results.json").write_text(json.dumps(results, indent=2) + "\n")
            print(scenario, mode, spreads, flush=True)
    results["complete"] = True
    (dest / "results.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
