#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
"""Build and capture source-matched native FV feasibility baselines (stdlib only)."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
CASES = ("open_slosh", "pressure_rest", "pressure_slosh", "filling", "pressure_flow",
         "tee_pressure", "loop_pressure", "mixed_reversed", "tee_rest", "loop_rest")
NETWORK_CASES = ("tee_pressure", "loop_pressure", "mixed_reversed", "tee_rest", "loop_rest")
REST_CASES = ("pressure_rest", "tee_rest", "loop_rest")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def schedule_digest(path: Path) -> str:
    """Compare accepted scheduling/physical ledger fields without wall time."""
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    for row in rows:
        del row["solver_wall_s"]
    return hashlib.sha256(json.dumps(rows, sort_keys=True).encode()).hexdigest()


def write_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def sources() -> dict[str, str]:
    names = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=ROOT,
    ).decode().split("\0")
    result = {}
    for name in sorted(set(names)):
        p = Path(name)
        relevant = name in ("CMakeLists.txt", "CMakePresets.json", ".gitignore") or name.startswith(
            ("src/", "include/", "cmake/", "tests/verification/fv_implicit_phase0/")
        ) or name in ("tests/unit/engine/fv_test_support.hpp",
                       "tests/unit/engine/test_fv_pressure_reference.cpp",
                       "tests/unit/engine/CMakeLists.txt")
        if relevant and (ROOT / p).is_file() and (
            p.suffix in (".c", ".cpp", ".h", ".hpp", ".cmake", ".json", ".in", ".py")
            or p.name in ("CMakeLists.txt", ".gitignore")
        ):
            result[name] = digest(ROOT / p)
    return result


def clean_environment() -> tuple[dict, dict]:
    env = dict(os.environ)
    removed = {}
    prefixes = ("OPENSWMM_", "SWMM_FV", "SWMM_DW", "OMP_", "KMP_", "GOMP_", "DYLD_")
    for key in list(env):
        if key.startswith(prefixes) or key in ("LD_LIBRARY_PATH", "LD_PRELOAD"):
            removed[key] = env.pop(key)
    env.update(OMP_NUM_THREADS="1", OMP_THREAD_LIMIT="1", OMP_DYNAMIC="FALSE",
               OMP_WAIT_POLICY="PASSIVE", KMP_BLOCKTIME="0")
    return env, removed


def capture(args, report: dict) -> None:
    build = args.build_dir.resolve()
    out = args.output.resolve()
    before = sources()
    report.update(source_before=before, head=subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        source_root=str(ROOT), build_dir=str(build), platform=platform.platform(),
        python=sys.version, scope="native component fixtures; not a full-engine performance qualification")
    write_json(out / "manifest.json", report)
    command = ["cmake", "--build", str(build), "--config", "Release",
               "--target", "fv_implicit_baseline", "--parallel", str(args.jobs)]
    report["build_command"] = command
    with (out / "build.log").open("w") as log:
        subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                       check=True)
    if sources() != before:
        raise RuntimeError("source inventory changed during build; recapture in a new output directory")
    binary = build / "bin" / "Release" / (
        "fv_implicit_baseline.exe" if os.name == "nt" else "fv_implicit_baseline")
    report["binary"] = {"path": str(binary), "sha256": digest(binary)}
    for name in ("CMakeCache.txt", "compile_commands.json"):
        source = build / name
        if not source.is_file():
            raise RuntimeError(f"missing {name}; configure with CMAKE_EXPORT_COMPILE_COMMANDS=ON")
        (out / name).write_bytes(source.read_bytes())
    # Preserve relevant source hashes and compile invocations, not just HEAD.
    commands = json.loads((out / "compile_commands.json").read_text())
    fv_commands = [c for c in commands if any(x in c["file"] for x in (
        "/hydraulics/fv/", "/fv_implicit_phase0/driver.cpp"))]
    if not fv_commands:
        raise RuntimeError("build has no native FV compile commands")
    for c in fv_commands:
        text = c.get("command", " ".join(c.get("arguments", [])))
        if os.name != "nt" and (
            "-ffast-math" in text or "-fno-fast-math" not in text
            or "-ffp-contract=off" not in text
        ):
            raise RuntimeError("FV build does not establish the strict floating-point contract")
    report["fv_compile_commands"] = fv_commands
    engine_path_file = build / "tests/verification/fv_implicit_phase0/engine_library_Release.txt"
    engine_library = Path(engine_path_file.read_text().strip()).resolve()
    report["local_runtime_libraries"] = {str(engine_library): digest(engine_library)}
    # Hashes identify a capture; retained bytes keep its evidence reviewable
    # after this mutable checkout/build advances to the next implementation.
    snapshot = out / "source_snapshot"
    for name, expected in before.items():
        dest = snapshot / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / name, dest)
        if digest(dest) != expected:
            raise RuntimeError(f"source changed while snapshotting: {name}")
    report["source_snapshot"] = "source_snapshot"
    artifacts = out / "artifact_snapshot"
    artifacts.mkdir()
    report["artifact_snapshot"] = {}
    for original in (binary, engine_library):
        dest = artifacts / original.name
        shutil.copyfile(original, dest)
        report["artifact_snapshot"][dest.name] = digest(dest)
    if platform.system() == "Darwin":
        report["linked_libraries"] = subprocess.check_output(
            ["otool", "-L", str(binary)], text=True)
        report["engine_linked_libraries"] = subprocess.check_output(
            ["otool", "-L", str(engine_library)], text=True)
    env, removed = clean_environment()
    report["cleared_environment"] = removed
    report["run_environment"] = {k: env[k] for k in env if k.startswith(("OMP_", "KMP_"))}
    report["configuration"] = {k: str(v) if isinstance(v, Path) else v
                               for k, v in vars(args).items()}
    rows = []
    groups = {}
    for case in args.cases:
        for cells in args.cells:
            for celerity in args.celerities:
                for mode in (0, 1):
                    key = (case, cells, celerity, mode)
                    group = []
                    for repetition in range(args.repeat):
                        name = f"{case}_n{cells}_c{celerity:g}_p{mode}_r{repetition}"
                        run_dir = out / name
                        cmd = [str(binary), case, str(cells), str(celerity), str(args.duration),
                               str(args.routing_step), str(mode), str(run_dir)]
                        with (out / f"{name}.log").open("w") as log:
                            subprocess.run(cmd, cwd=ROOT, env=env, stdout=log,
                                           stderr=subprocess.STDOUT, timeout=args.timeout, check=True)
                        row = json.loads((run_dir / "summary.json").read_text())
                        if row["schema_version"] != 2:
                            raise RuntimeError("native driver summary schema is not current")
                        row["run"] = name
                        row["command"] = cmd
                        row["mesh_sha256"] = digest(run_dir / "mesh_before.csv")
                        row["states_sha256"] = digest(run_dir / "states.csv")
                        row["nodes_sha256"] = digest(run_dir / "nodes.csv")
                        row["schedule_sha256"] = schedule_digest(run_dir / "routing_steps.csv")
                        if row["mesh_sha256"] != digest(run_dir / "mesh_after.csv"):
                            raise RuntimeError(f"physical mesh mutated in {name}")
                        if group and row["states_sha256"] != group[0]["states_sha256"]:
                            raise RuntimeError(f"repeat state mismatch in {name}")
                        if group and row["nodes_sha256"] != group[0]["nodes_sha256"]:
                            raise RuntimeError(f"repeat node/carry mismatch in {name}")
                        if group and row["schedule_sha256"] != group[0]["schedule_sha256"]:
                            raise RuntimeError(f"repeat schedule/ledger mismatch in {name}")
                        if row["max_relative_balance_defect"] > 1.e-10:
                            raise RuntimeError(f"cell/node/boundary conservation failed in {name}")
                        if case in REST_CASES and (
                            row["max_abs_discharge_cfs"] > 1.e-9 or row["max_depth_change_ft"] > 1.e-9
                            or row["sampled_max_node_head_change_ft"] > 1.e-9
                        ):
                            raise RuntimeError(f"pressurized rest state drifted in {name}")
                        if case in NETWORK_CASES:
                            if row["sampled_max_symmetry_diff_cfs"] > 1.e-8 * max(1., row["max_abs_discharge_cfs"]):
                                raise RuntimeError(f"network symmetry failed in {name}")
                            if mode and row["sampled_max_folded_degree"] < 3:
                                raise RuntimeError(f"branched pressure classification not witnessed in {name}")
                            if row["flood_volume_ft3"] != 0:
                                raise RuntimeError(f"unexpected flooding in pressurized network {name}")
                        if case == "mixed_reversed":
                            q = row["final_conduit_discharge_cfs"]
                            if not (q[0] > 0 and q[1] > 0 and q[2] < 0):
                                raise RuntimeError(f"reversed-conduit flow direction failed in {name}")
                        group.append(row)
                        rows.append(row)
                        write_json(out / "runs.json", rows)
                    groups[key] = group
    comparisons = []
    for key, explicit in groups.items():
        case, cells, celerity, mode = key
        if mode:
            continue
        pressure = groups[(case, cells, celerity, 1)]
        if explicit[0]["mesh_sha256"] != pressure[0]["mesh_sha256"]:
            raise RuntimeError(f"mesh mismatch between modes: {key}")
        if case == "open_slosh" and explicit[0]["states_sha256"] != pressure[0]["states_sha256"]:
            raise RuntimeError(f"pressure option is not inert in free-surface case: {key}")
        comparisons.append(dict(case=case, cells=explicit[0]["cells"], cells_per_conduit=cells,
            celerity_ft_s=celerity,
            mesh_equal=True, state_equal=explicit[0]["states_sha256"] == pressure[0]["states_sha256"],
            explicit_internal_steps=explicit[0]["internal_steps"],
            pressure_internal_steps=pressure[0]["internal_steps"],
            explicit_solver_wall_median_s=statistics.median(x["solver_wall_s"] for x in explicit),
            pressure_solver_wall_median_s=statistics.median(x["solver_wall_s"] for x in pressure)))
    report["source_after"] = sources()
    if report["source_after"] != before:
        raise RuntimeError("source inventory changed during capture; results are not source matched")
    if digest(binary) != report["binary"]["sha256"]:
        raise RuntimeError("baseline binary changed during capture")
    for path, sha in report["local_runtime_libraries"].items():
        if digest(Path(path)) != sha:
            raise RuntimeError(f"runtime library changed during capture: {path}")
    write_json(out / "comparisons.json", comparisons)
    report["status"] = "passed"
    report["run_count"] = len(rows)
    report["limits"] = [
        "No speedup acceptance claim: short instrumented fixtures on an uncontrolled host.",
        "Global Euler stepping only; no LTS, RK2, TPA, structures, quality or GUI qualification.",
        "Regime changes/transition counts are routing-endpoint samples, not internal front histories.",
        "Argmin counters are the native solver census counts; not transition-face time attribution.",
        "Inventory includes nonprescribed node volume, negative carry and exported flooding; no structures/laterals in these fixtures.",
        "Folded degree-three masks witness the non-path pressure-solver route by source inspection; no CG iteration histories yet.",
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cases", nargs="+", choices=CASES, default=list(CASES))
    parser.add_argument("--cells", nargs="+", type=int, default=[4, 8, 16])
    parser.add_argument("--celerities", nargs="+", type=float, default=[100., 300.])
    parser.add_argument("--duration", type=float, default=30.)
    parser.add_argument("--routing-step", type=float, default=5.)
    parser.add_argument("--repeat", type=int, default=2)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=120.)
    args = parser.parse_args()
    if args.repeat < 2:
        parser.error("at least two repetitions are required for the determinism check")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    report = {"schema_version": 1, "status": "running"}
    try:
        capture(args, report)
    except Exception as error:
        report.update(status="failed", error=str(error))
        print(f"baseline capture failed: {error}", file=sys.stderr)
        return 1
    finally:
        write_json(args.output / "manifest.json", report)
    print(f"Captured and checked {report['run_count']} runs in {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
