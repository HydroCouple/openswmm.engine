#!/usr/bin/env python3
"""Compare independent and shared GW windows at common report times (§9.3)."""

import argparse
from itertools import combinations
import json
import os
from pathlib import Path
import re
import subprocess
import h5py
import numpy as np

ROOT = Path(__file__).resolve().parents[3]


def model(case, mode, window, cfl):
    folder = ROOT / "tests/benchmarks/generated/gw2d_clock"
    text = (folder / "storm_mixed.inp").read_text()
    if case in ("dry", "long_recession"):
        text = text[: text.index("[2D_AQUIFER]")] + (
            "[2D_AQUIFER]\nCELL 1 360 2 .45 .1 2 HG0 1.01 C_LOSS 0.2 CLOSURE ENSLAVED\n"
            "CELL 2 360 2 .45 .1 2 HG0 .99 C_LOSS 0.4 CLOSURE ENSLAVED\n"
            "[2D_AQUIFER_BOUNDARIES]\nCELL 1 EDGE 0 HEAD CONST -1.2\n"
        )
    if case == "dry":
        text = re.sub(r"\[RAINGAGES\].*?(?=\[JUNCTIONS\])", "", text, flags=re.S)
        text = text.replace("RAINFALL_MODE NEAREST_NEIGHBOUR", "").replace(
            ".03 .005", ".03 0"
        )
    elif case == "long_recession":
        text = text.replace("END_DATE 01/01/2026", "END_DATE 01/02/2026")
        text = text.replace("END_TIME 01:00:00", "END_TIME 00:10:00")
        text = text.replace("REPORT_STEP 00:01:00", "REPORT_STEP 00:10:00")
        text = text.replace("GW_ET NONE", "GW_ET BOTH").replace(
            "EVAPORATION NO", "EVAPORATION CLIMATE"
        )
        text += "\n[EVAPORATION]\nCONSTANT 2\n"
    elif case.startswith("coupled"):
        text = text.replace("J1 -10 1", "J1 -1.2 2 0.25").replace(
            "MAX_TIMESTEP 3",
            f"MAX_TIMESTEP {60 if case == 'coupled_wide' else 3 * cfl / 0.7}",
        )
        text += (
            "\n[OUTFALLS]\nO1 -1.3 FREE\n[CONDUITS]\nC1 J1 O1 25 .015 0 0 0 0\n"
            "[XSECTIONS]\nC1 CIRCULAR .3 0 0 0 1\n[COORDINATES]\nJ1 3 3\nO1 30 3\n"
            "[2D_AQUIFER_NODE]\nJ1 1 KC 36 DC .1 AREA .5\n"
        )
        if case == "coupled_overflow":
            # Sustain physical saturation excess in the bulk cell independently of recharge.
            text += "\n[GW_SOURCES]\nWELL CELL 2 FLOW 0.0002\n"
    text = text.replace("ROUTING_STEP 10", f"ROUTING_STEP {window}")
    text = text.replace(
        "[2D_AQUIFER_OPTIONS]", f"[2D_AQUIFER_OPTIONS]\nGW_CLOCK {mode}"
    )
    return text.replace(
        "[2D_OPTIONS]",
        f"[2D_OPTIONS]\nCFL_NUMBER {cfl}\nBACKEND CPU\n"
        "OUTPUT_FILE fields.2d.h5\nOUTPUT_PRECISION FLOAT64\nREPORT_2D_VARIABLES ALL",
    )


def run(
    cli,
    root,
    case,
    mode,
    window,
    cfl,
    *,
    environment=None,
    fixed_windows=False,
    gw_max_step=None,
    gw_options=None,
    surface_max_step=None,
):
    dest = root / f"{case}_{mode}_{window}_{cfl}"
    dest.mkdir()
    deck = model(case, mode, window, cfl)
    if gw_options:
        deck = deck.replace(
            "GW_CLOCK INDEPENDENT\n",
            "GW_CLOCK INDEPENDENT\n"
            + "".join(f"{k} {v}\n" for k, v in gw_options.items()),
        )
    if surface_max_step is not None:
        if not np.isfinite(surface_max_step) or surface_max_step <= 0:
            raise ValueError("The surface ceiling must be finite and positive")
        deck = re.sub(
            r"MAX_TIMESTEP [\d.]+", f"MAX_TIMESTEP {surface_max_step:.17g}", deck
        )
    if gw_max_step is not None and mode == "INDEPENDENT":
        if not np.isfinite(gw_max_step) or gw_max_step <= 0:
            raise ValueError("The groundwater ceiling must be finite and positive")
        deck = deck.replace(
            "GW_CLOCK INDEPENDENT\n",
            f"GW_CLOCK INDEPENDENT\nGW_MAX_STEP {gw_max_step:.17g}\n",
        )
    if fixed_windows:
        # The original ten-second WET_STEP silently caps ROUTING_STEP 30 at ten.
        # Preserve the aquifer intake interval while exercising three actual window sizes.
        deck = deck.replace("WET_STEP 00:00:10", "WET_STEP 00:00:30")
        deck = deck.replace("DRY_STEP 00:00:10", "DRY_STEP 00:00:30")
        deck = deck.replace("[2D_OPTIONS]", "[2D_OPTIONS]\nINFIL_STEP 10")
        deck = deck.replace("[OPTIONS]", "[OPTIONS]\nVARIABLE_STEP 0")
    (dest / "model.inp").write_text(deck)
    env = dict(os.environ, OPENSWMM_PERF="1", OPENSWMM_2D_MARCHER_CHECK="1")
    if environment:
        env.update(environment)
    with (dest / "console.log").open("w") as f:
        subprocess.run(
            [cli, "model.inp", "model.rpt", "model.out"],
            cwd=dest,
            env=env,
            stdout=f,
            stderr=subprocess.STDOUT,
            check=True,
        )
    report = (dest / "model.rpt").read_bytes()
    if b"Unknown section" in report or b"ERROR " in report:
        raise ValueError(f"Unaccepted qualification deck: {dest}")
    if fixed_windows:
        duration = 87000 if case == "long_recession" else 3600
        count = re.search(
            rb"(\d+) steps completed", (dest / "console.log").read_bytes()
        )
        if not count or int(count[1]) * window != duration:
            raise ValueError(f"Unexpected executed step count for {window} s in {dest}")
    if fixed_windows and case.startswith(
        "coupled"
    ):  # Dry cases omit hydraulic statistics.
        for stat in (b"Minimum", b"Average", b"Maximum"):
            match = re.search(stat + rb" Time Step\s*:\s*([\d.]+) sec", report)
            if not match or float(match[1]) != window:
                raise ValueError(
                    f"Expected a fixed {window} s hydraulic step in {dest}"
                )
    with h5py.File(dest / "fields.2d.h5") as h:
        if case == "dry" and np.any(h["Mesh2_face_depth"][:] != 0):
            raise ValueError("Dry qualification unexpectedly wetted the surface")
        return {
            k: h[v][:]
            for k, v in [
                ("time", "time"),
                ("head", "Mesh2_face_gw_hg"),
                ("return", "Mesh2_face_gw_dunne_cum"),
            ]
        }


def measure(rows):
    result = {}
    for field in ("head", "return"):
        pairs = list(combinations(rows, 2))
        assert all(np.array_equal(a["time"], b["time"]) for a, b in pairs)
        deltas = [a[field].astype("float64") - b[field] for a, b in pairs]
        assert all(np.isfinite(delta).all() for delta in deltas)
        result[field] = {
            "max": max(float(np.max(np.abs(x))) for x in deltas),
            "rms": max(float(np.sqrt(np.mean(x * x))) for x in deltas),
        }
    return result


def relative_l2(values, reference):
    error = np.linalg.norm(values - reference)
    scale = np.linalg.norm(reference)
    if scale > 0:
        return float(error / scale)
    return 0.0 if error == 0 else float("inf")


def long_recession(cli, root, fixed_windows=False, gw_max_step=None):
    shared = run(
        cli, root, "long_recession", "SHARED", 10, 0.7, fixed_windows=fixed_windows
    )
    independent = run(
        cli,
        root,
        "long_recession",
        "INDEPENDENT",
        10,
        0.7,
        fixed_windows=fixed_windows,
        gw_max_step=gw_max_step,
    )
    assert np.array_equal(independent["time"], shared["time"])
    # Reports begin at the end of the ten-minute storm and span the next day.
    losses = {
        mode: (row["head"][0] - row["head"][-1]).tolist()
        for mode, row in (("independent", independent), ("shared", shared))
    }
    errors = {
        field: relative_l2(independent[field], shared[field])
        for field in ("head", "return")
    }
    gates = {
        "long_recession.recedes": all(min(values) > 0 for values in losses.values()),
        "long_recession.head": errors["head"] <= 0.01,
        "long_recession.return": errors["return"] <= 0.02,
    }
    return {"relative_l2": errors, "head_drop_m": losses}, gates


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--gw-max-step",
        type=float,
        help="Override the independent groundwater ceiling in seconds",
    )
    parser.add_argument(
        "--fixed-windows",
        action="store_true",
        help="Require measured hydraulic steps of 1, 10, and 30 seconds",
    )
    a = parser.parse_args()
    cli = a.engine.resolve()
    root = a.output.resolve()
    root.mkdir(exist_ok=False)
    results = {
        "fixed_windows": a.fixed_windows,
        "verification_scope": "Cross-window allowances and dry/recession accuracy; A2 producer convergence is a separate required suite.",
    }
    if a.gw_max_step is not None:
        results["gw_max_step_s"] = a.gw_max_step
    dry_rows = {}
    coupled_cases = ("coupled", "coupled_wide", "coupled_overflow")
    for case in ("dry", *coupled_cases):
        for cfl in (0.7,) if case == "dry" else (0.7, 0.35):
            for mode in ("SHARED", "INDEPENDENT"):
                rows = [
                    run(
                        cli,
                        root,
                        case,
                        mode,
                        w,
                        cfl,
                        fixed_windows=a.fixed_windows,
                        gw_max_step=a.gw_max_step,
                    )
                    for w in (1, 10, 30)
                ]
                diff = measure(rows)
                if case == "coupled_overflow":
                    volumes = [float(row["return"][-1, 1]) for row in rows]
                    if min(volumes) <= 0.01:
                        raise ValueError(
                            "Sustained-inflow control produced too little surface return"
                        )
                    diff["final_return_m3"] = volumes
                if case == "dry":
                    dry_rows[mode] = rows
                    diff["identical"] = all(
                        rows[0][key].tobytes() == r[key].tobytes()
                        for key in ("head", "return")
                        for r in rows[1:]
                    )
                    diff["evolving"] = bool(
                        np.any(rows[0]["head"][0] != rows[0]["head"][-1])
                    )
                results[f"{case}.{cfl}.{mode}"] = diff
                (root / "results.json").write_text(json.dumps(results, indent=2) + "\n")
                print(case, cfl, mode, diff, flush=True)
    dry = results["dry.0.7.INDEPENDENT"]
    gates = {"quiescent": dry["evolving"] and dry["identical"]}
    dry_error = {}
    for field, limit in [("head", 0.01), ("return", 0.02)]:
        errors = []
        for independent, shared in zip(dry_rows["INDEPENDENT"], dry_rows["SHARED"]):
            assert np.array_equal(independent["time"], shared["time"])
            errors.append(relative_l2(independent[field], shared[field]))
        dry_error[field] = max(errors)
        gates[f"quiescent.{field}.shared_relative_l2"] = max(errors) <= limit
    results["dry.shared_relative_l2"] = dry_error
    results["long_recession"], recession_gates = long_recession(
        cli, root, a.fixed_windows, a.gw_max_step
    )
    gates.update(recession_gates)
    for case in coupled_cases:
        for field in ("head", "return"):
            for norm in ("max", "rms"):
                coarse = results[f"{case}.0.7.INDEPENDENT"][field][norm]
                fine = results[f"{case}.0.35.INDEPENDENT"][field][norm]
                # A2 uses actual per-window producer-step refinement in
                # gw_clock_qualification.py; retain this historical comparison
                # as evidence, without treating a CFL label as an executed step.
                results[f"{case}.{field}.{norm}.cross_window_spread_decreased"] = (
                    fine < coarse or fine == coarse == 0
                )
                for cfl in (0.35, 0.7):
                    gates[f"{case}.{cfl}.{field}.{norm}.relative"] = (
                        results[f"{case}.{cfl}.INDEPENDENT"][field][norm]
                        <= 2 * results[f"{case}.{cfl}.SHARED"][field][norm]
                    )
    results["gates"] = gates
    (root / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    if not all(gates.values()):
        raise SystemExit("Window sensitivity gate failed; see results.json")


if __name__ == "__main__":
    main()
