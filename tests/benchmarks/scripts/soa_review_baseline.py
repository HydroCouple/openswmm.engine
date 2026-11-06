#!/usr/bin/env python3
"""Run matched CLI builds and gate process instrumentation on exact output identity.

Requires h5py/numpy. Copies standalone fixtures into isolated, retained run
directories. Reports on/off parity and diagnostic (not acceptance) wall times.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

import h5py
import numpy as np


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def engine(cli):
    for folder in (cli.resolve().parent, cli.resolve().parent.parent / "engine"):
        for pattern in ("libopenswmm.engine*.dylib", "libopenswmm.engine*.so", "openswmm.engine*.dll"):
            for library in sorted(folder.glob(pattern)):
                return {"path": str(library.resolve()), "sha256": sha(library)}
    raise RuntimeError(f"Cannot resolve engine library for {cli}")


def datasets(path):
    result = {}
    with h5py.File(path, "r") as source:
        def visit(name, item):
            if not isinstance(item, h5py.Dataset):
                return
            data = np.asarray(item[()])
            content = repr(data.tolist()).encode() if data.dtype.hasobject else data.tobytes()
            result[name] = {"shape": item.shape, "dtype": str(item.dtype),
                            "sha256": hashlib.sha256(content).hexdigest()}
        source.visititems(visit)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path)
    parser.add_argument("patched", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--warmup", type=int, default=0)
    parser.add_argument("--malloc-probe", type=Path)
    parser.add_argument("--fixtures", type=Path)
    parser.add_argument("--pattern", default="*.inp")
    args = parser.parse_args()
    if args.repeat < 1 or args.warmup < 0:
        parser.error("--repeat must be positive and --warmup nonnegative")
    root = Path(__file__).resolve().parents[3]
    fixtures = args.fixtures or root / "tests/benchmarks/generated/soa_review_2026-10"
    decks = sorted(fixtures.glob(args.pattern))
    if not decks:
        parser.error("no fixture decks matched")
    provenance = {
        "base": str(args.base.resolve()), "base_cli_sha256": sha(args.base),
        "patched": str(args.patched.resolve()), "patched_cli_sha256": sha(args.patched),
        "base_engine": engine(args.base), "patched_engine": engine(args.patched),
        "fixtures": {p.name: sha(p) for p in decks},
        "warmup_repeats": args.warmup,
        "timing_order": "alternate baseline/candidate profiling-off order; profiled run last",
        "hdf5_comparison": "all datasets, dtype + shape + exact values; file metadata excluded",
    }
    if provenance["base_engine"]["sha256"] == provenance["patched_engine"]["sha256"]:
        parser.error("the base and patched engine libraries are identical")
    # Refuse reuse: a partial rerun must never inherit old results.
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    (output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    results = []
    warmups = []
    for inp in decks:
        for repeat in range(-args.warmup, args.repeat):
            runs = {}
            sides = [("base", args.base, False), ("patched", args.patched, False)]
            if repeat % 2:
                sides.reverse()
            sides.append(("profiled", args.patched, True))
            for side, cli, perf in sides:
                dest = output / inp.stem / str(repeat) / side
                dest.mkdir(parents=True)
                text = inp.read_text()
                has_mesh = "[2D_VERTICES]" in text or "[2D_MESH_FILE]" in text
                if has_mesh:
                    text += ("\n[2D_OPTIONS]\nBACKEND CPU\nREPORT_2D YES\n"
                             "REPORT_2D_VARIABLES ALL\nOUTPUT_FILE fields.2d.h5\n")
                deck = dest / inp.name
                deck.write_text(text)
                # The large-case preparer retains dependency files beside each
                # deck. Link these read-only inputs rather than duplicating a
                # multi-decade rain series in every repeat.
                for sibling in inp.parent.iterdir():
                    if sibling.is_file() and sibling.suffix.lower() in {".dat", ".2dm", ".heat"}:
                        (dest / sibling.name).symlink_to(sibling.resolve())
                env = os.environ.copy()
                env.pop("OPENSWMM_PERF", None)
                env.pop("DYLD_INSERT_LIBRARIES", None)
                if perf:
                    env["OPENSWMM_PERF"] = "1"
                    if args.malloc_probe:
                        env["DYLD_INSERT_LIBRARIES"] = str(args.malloc_probe.resolve())
                start = time.perf_counter()
                with (dest / "stdout.log").open("w") as log:
                    completed = subprocess.run(
                        [str(cli.resolve()), str(deck), "model.rpt", "model.out"],
                        cwd=dest, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=600)
                wall = time.perf_counter() - start
                if completed.returncode or not (dest / "model.out").is_file():
                    raise RuntimeError(f"Failed {dest}; see stdout.log")
                fields = datasets(dest / "fields.2d.h5") if has_mesh else {}
                if has_mesh and not fields:
                    raise RuntimeError(f"No HDF5 datasets in {dest}")
                stdout = (dest / "stdout.log").read_text()
                phases = {}
                for group, body in re.findall(r"\[PERF-([^]]+)\]([^\n]+)", stdout):
                    phases[group] = dict(re.findall(r"([\w.]+)=([^\s]+)", body))
                if "PROCESS-INVALID" in phases:
                    raise RuntimeError(f"Invalid profiling attribution in {dest}; see stdout.log")
                if perf and not ({"2D", "GW2D", "HYD"} & phases.keys()):
                    raise RuntimeError(f"No process timers reached in {dest}")
                if not perf and phases:
                    raise RuntimeError(f"Profiling leaked into disabled run {dest}")
                if perf and args.malloc_probe and "ALLOC" not in phases:
                    raise RuntimeError(f"Malloc probe was not loaded in {dest}")
                runs[side] = {"wall_seconds": wall, "out_sha256": sha(dest / "model.out"),
                              "datasets": fields, "phases": phases}
            for side in ("patched", "profiled"):
                if runs[side]["out_sha256"] != runs["base"]["out_sha256"]:
                    raise RuntimeError(f".out differs for {inp.stem}: {side}")
                if runs[side]["datasets"] != runs["base"]["datasets"]:
                    raise RuntimeError(f"HDF5 datasets differ for {inp.stem}: {side}")
            row = {"deck": inp.name, "repeat": repeat, "runs": runs}
            (warmups if repeat < 0 else results).append(row)
            (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
            (output / "warmups.json").write_text(json.dumps(warmups, indent=2) + "\n")
            label = f"warmup {repeat}" if repeat < 0 else f"#{repeat}"
            print(f"{inp.stem} {label}: exact .out/HDF5 identity (profiling off/on)", flush=True)


if __name__ == "__main__":
    main()
