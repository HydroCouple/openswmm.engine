#!/usr/bin/env python3
"""Extend the Phase 2 envelope to authored SIGMA, mixed, and thermal fixtures.

Keep producer settings unchanged. The required reference is INDEPENDENT with
GW_MAX_STEP=1 s; 0.25/0.0625 s references expose its remaining temporal error.
All inputs, full-precision fields, reports, logs, and provenance are retained.
"""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time

import h5py
import numpy as np

from gw_clock_baseline import clock_rows
from gw_clock_qualification import report_stats
from soa_review_baseline import engine, sha

ROOT = Path(__file__).resolve().parents[3]
FIXTURES = ROOT / "tests/benchmarks/generated/soa_review_2026-10"
CASES = {
    "sigma": FIXTURES / "soa_sigma.inp",
    "mixed": FIXTURES / "pipeline_parallel/mixed_water_t1.inp",
    "species_heat": FIXTURES / "soa_gw_species_heat.inp",
}
CLOSURES = {0: "CLOSED_FORM", 1: "ENSLAVED", 2: "SIGMA"}


def deck(source, mode, ceiling, beta, epsilon, surface_max_step=None, base_step=1):
    text = source.read_text()
    keys = (
        "THREADS|BACKEND|OUTPUT_FILE|OUTPUT_PRECISION|REPORT_2D_VARIABLES|"
        "GW_CLOCK|GW_MAX_STEP|GW_BASE_STEP|GW_BUDGET_FRACTION|GW_HEAD_TOLERANCE|"
        "GW_BINDS_SURFACE_DT"
    )
    text = re.sub(rf"^\s*(?:{keys})\s+[^\n]*\n", "", text, flags=re.M)
    text = text.replace("[OPTIONS]", "[OPTIONS]\nTHREADS 1")
    if surface_max_step is not None:
        text = re.sub(
            r"^MAX_TIMESTEP\s+[^\n]+",
            f"MAX_TIMESTEP {surface_max_step}",
            text,
            flags=re.M,
        )
    text = text.replace(
        "[2D_OPTIONS]",
        "[2D_OPTIONS]\nBACKEND CPU\nOUTPUT_FILE fields.2d.h5\n"
        "OUTPUT_PRECISION FLOAT64\nREPORT_2D_VARIABLES ALL",
    )
    return text.replace(
        "[2D_AQUIFER_OPTIONS]",
        f"[2D_AQUIFER_OPTIONS]\nGW_CLOCK {mode}\nGW_BASE_STEP {base_step}\n"
        f"GW_MAX_STEP {ceiling}\nGW_BUDGET_FRACTION {beta}\n"
        f"GW_HEAD_TOLERANCE {epsilon}\n",
    )


def residuals(h):
    result = {}
    for name in (
        "groundwater_ledger",
        "groundwater_species_ledger",
        "ground_heat_budget",
    ):
        if name not in h:
            continue
        data = h[name]
        terms = data.attrs["terms"].decode().split(",")
        column = next(i for i, term in enumerate(terms) if "residual" in term)
        values = data[:]
        if name == "groundwater_species_ledger":
            # The temperature pseudo-species ledger does not include grain
            # heat. Physical thermal closure is ground_heat_budget in joules.
            species = (
                h["Mesh2_face_gw_sat_conc"].attrs["species_names"].decode().split(",")
            )
            result[name] = {
                s: float(np.max(np.abs(values[:, i, column])))
                for i, s in enumerate(species)
                if s != "__TEMPERATURE__"
            }
        else:
            residual = values[..., column]
            result[name] = {"max_abs": float(np.max(np.abs(residual)))}
            if name == "ground_heat_budget":
                initial = np.abs(values[..., terms.index("initial_j")])
                result[name]["max_relative_to_initial"] = float(
                    np.max(np.abs(residual) / np.maximum(initial, 1.0))
                )
    if "Mesh2_face_soil_layer_temperature" in h and "Mesh2_face_gw_sat_conc" in h:
        names = h["Mesh2_face_gw_sat_conc"].attrs["species_names"].decode().split(",")
        if "__TEMPERATURE__" in names:
            row = names.index("__TEMPERATURE__")
            layers = h["Mesh2_face_soil_layer_temperature"][:]
            lower, upper = layers.min(axis=1), layers.max(axis=1)
            # Diagnostic only: compare the legacy water-temperature view to
            # the range of physical layer temperatures at the same report.
            result["legacy_temperature_outside_layer_range_degC"] = {}
            for zone in ("sat", "unsat"):
                values = h[f"Mesh2_face_gw_{zone}_conc"][:, row, :]
                excess = np.maximum(np.maximum(lower - values, values - upper), 0)
                result["legacy_temperature_outside_layer_range_degC"][zone] = float(
                    excess.max()
                )
    return result


def read(path):
    with h5py.File(path) as h:
        fields = {
            "head": "Mesh2_face_gw_hg",
            "return": "Mesh2_face_gw_dunne_cum",
            "sat_concentration": "Mesh2_face_gw_sat_conc",
            "unsat_concentration": "Mesh2_face_gw_unsat_conc",
            "temperature": "Mesh2_face_soil_temperature",
            "layer_temperature": "Mesh2_face_soil_layer_temperature",
        }
        row = {key: h[name][:] for key, name in fields.items() if name in h}
        for key, values in row.items():
            assert values.dtype == np.float64, (path, key, values.dtype)
            assert np.isfinite(values).all(), (path, key)
        row["time"] = h["time"][:]
        row["closure"] = h["Mesh2_face_gw_closure"][:]
        if "sat_concentration" in row:
            row["species"] = (
                h["Mesh2_face_gw_sat_conc"].attrs["species_names"].decode().split(",")
            )
        return row, residuals(h)


def errors(row, ref):
    np.testing.assert_array_equal(row["time"], ref["time"])
    np.testing.assert_array_equal(row["closure"], ref["closure"])
    result = {}
    for key in (
        "head",
        "return",
        "sat_concentration",
        "unsat_concentration",
        "temperature",
        "layer_temperature",
    ):
        if key not in row:
            continue
        a, b = row[key], ref[key]
        delta = a - b
        scale = np.linalg.norm(b)
        result[key] = {
            "max_abs": float(np.max(np.abs(delta))),
            "rms": float(np.sqrt(np.mean(delta**2))),
            "relative_l2": float(np.linalg.norm(delta) / scale) if scale else None,
            "exact": bool(np.array_equal(a, b)),
        }
        if "concentration" in key:
            result[key]["by_species"] = {}
            for i, species in enumerate(ref["species"]):
                norm = np.linalg.norm(b[:, i])
                result[key]["by_species"][species] = {
                    "max_abs": float(np.max(np.abs(delta[:, i]))),
                    "relative_l2": float(np.linalg.norm(delta[:, i]) / norm)
                    if norm
                    else None,
                }
        if key == "head":
            result[key]["by_closure"] = {}
            for code, name in CLOSURES.items():
                mask = ref["closure"] == code
                if not mask.any():
                    continue
                result[key]["by_closure"][name] = float(
                    np.linalg.norm(delta[:, mask]) / np.linalg.norm(b[:, mask])
                )
            index = np.unravel_index(np.argmax(np.abs(delta)), delta.shape)
            result[key]["worst"] = {
                "swmm_date_days": float(ref["time"][index[0]]),
                "cell_1based": int(index[1]) + 1,
                "value": float(a[index]),
                "reference": float(b[index]),
            }
    return result


def run(
    cli,
    dest,
    source,
    mode="INDEPENDENT",
    ceiling=3600,
    beta=0.5,
    epsilon=0.05,
    surface_max_step=None,
    base_step=1,
):
    dest.mkdir(parents=True)
    (dest / "model.inp").write_text(
        deck(source, mode, ceiling, beta, epsilon, surface_max_step, base_step)
    )
    env = {
        k: v
        for k, v in os.environ.items()
        if not k.startswith(("OPENSWMM_GW_STUDY", "OPENSWMM_GW_CONTROL"))
    }
    env.update(OPENSWMM_PERF="1", OPENSWMM_2D_MARCHER_CHECK="1")
    start = time.monotonic()
    with (dest / "console.log").open("w") as log:
        subprocess.run(
            [str(cli), "model.inp", "model.rpt", "model.out"],
            cwd=dest,
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
        )
    report = (dest / "model.rpt").read_text(encoding="latin-1")
    if "Unknown section" in report or "ERROR " in report:
        raise ValueError(f"Unaccepted fixture: {dest}")
    data, budgets = read(dest / "fields.2d.h5")
    stats = {
        "wall_seconds": time.monotonic() - start,
        "clock": clock_rows((dest / "console.log").read_text()),
        "residuals": budgets,
        "input_sha256": sha(dest / "model.inp"),
        "hdf5_sha256": sha(dest / "fields.2d.h5"),
    }
    if mode == "INDEPENDENT":
        stats.update(report_stats(dest / "model.rpt"))
    return data, stats


def relative(error):
    # A zero reference has no relative norm. Require exactly zero output.
    return (
        error["relative_l2"]
        if error["relative_l2"] is not None
        else (0.0 if error["exact"] else float("inf"))
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--cases", nargs="+", choices=CASES, default=list(CASES))
    parser.add_argument(
        "--gw-base-step",
        type=float,
        default=1,
        help="authored groundwater base cadence in seconds",
    )
    parser.add_argument(
        "--surface-max-step",
        type=float,
        help="explicit producer-refinement control, seconds",
    )
    args = parser.parse_args()
    if not np.isfinite(args.gw_base_step) or args.gw_base_step <= 0:
        parser.error("--gw-base-step must be finite and positive")
    if args.surface_max_step is not None and (
        not np.isfinite(args.surface_max_step) or args.surface_max_step <= 0
    ):
        parser.error("--surface-max-step must be finite and positive")
    cli = args.engine.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    result = {
        "engine": engine(cli),
        "cli_sha256": sha(cli),
        "script_sha256": sha(Path(__file__)),
        "surface_max_step_override": args.surface_max_step,
        "gw_base_step": args.gw_base_step,
        "reference": "INDEPENDENT GW_MAX_STEP=1; same authored base and producer settings",
        "inputs": {
            name: {"path": str(CASES[name]), "sha256": sha(CASES[name])}
            for name in args.cases
        },
        "cases": {},
        "gates": {},
    }

    def save():
        (output / "results.json").write_text(
            json.dumps(result, indent=2, allow_nan=False) + "\n"
        )

    save()
    for case in args.cases:
        source = CASES[case]
        rows, stats = {}, {}
        record = result["cases"][case] = {"ceilings": {}, "tolerances": {}}
        for ceiling in (1, 0.25, 0.0625, 10, 60, 600, 3600):
            row, stat = run(
                cli,
                output / case / f"ceiling_{ceiling}",
                source,
                ceiling=ceiling,
                surface_max_step=args.surface_max_step,
                base_step=args.gw_base_step,
            )
            rows[ceiling], stats[ceiling] = row, stat
            record["ceilings"][str(ceiling)] = {
                "errors_vs_1s": errors(row, rows[1]),
                "executed": stat,
            }
            print(
                case,
                ceiling,
                record["ceilings"][str(ceiling)]["errors_vs_1s"]["head"],
                flush=True,
            )
            save()
        record["reference_convergence"] = {
            "1_vs_0.0625": errors(rows[1], rows[0.0625]),
            "0.25_vs_0.0625": errors(rows[0.25], rows[0.0625]),
        }
        shared, stat = run(
            cli,
            output / case / "shared",
            source,
            mode="SHARED",
            surface_max_step=args.surface_max_step,
            base_step=args.gw_base_step,
        )
        record["shared"] = {"errors_vs_1s": errors(shared, rows[1]), "executed": stat}
        for field, limit in (("head", 0.01), ("return", 0.02)):
            values = [
                relative(record["ceilings"][str(c)]["errors_vs_1s"][field])
                for c in (1, 10, 60, 600, 3600)
            ]
            result["gates"][f"{case}.{field}.default_accuracy"] = values[-1] <= limit
            result["gates"][f"{case}.{field}.ceiling_monotonic"] = all(
                fine <= coarse + 1e-12 for fine, coarse in zip(values, values[1:])
            )
        for beta in (0.1, 0.5, 0.9):
            for epsilon in (0.01, 0.05, 0.2):
                if (beta, epsilon) == (0.5, 0.05):
                    row, stat = rows[3600], stats[3600]
                else:
                    row, stat = run(
                        cli,
                        output / case / f"budget_{beta}_{epsilon}",
                        source,
                        beta=beta,
                        epsilon=epsilon,
                        surface_max_step=args.surface_max_step,
                        base_step=args.gw_base_step,
                    )
                record["tolerances"][f"{beta},{epsilon}"] = {
                    "errors_vs_1s": errors(row, rows[1]),
                    "executed": stat,
                }
                save()
        print(
            case,
            "gates",
            {k: v for k, v in result["gates"].items() if k.startswith(case + ".")},
            flush=True,
        )
    save()
    print("failed gates:", [k for k, v in result["gates"].items() if not v], flush=True)
    return 0 if all(result["gates"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
