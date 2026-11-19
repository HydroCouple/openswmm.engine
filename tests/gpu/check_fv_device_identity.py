"""Require the 1D finite volume device backend to reproduce the CPU solver bit for bit.

The deck is run twice with the options the device marcher integrates (global stepping, every
cell and face active, structure and boundary forcing held over the routing step): once on the CPU solver with the
snapshot residual rule the marcher uses, once with the substep loop on the plugin. The two runs
must write the same binary output, the same end-of-run hot start file (double precision), and the
same report apart from wall-clock lines and the CPU-only work-list telemetry.

With --expect-refusal the plugin run must instead name the unsupported item and fall back to the
CPU solver.
"""

import argparse
import filecmp
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

# Report lines that legitimately differ: wall clock, the name of the backend
# that ran the substeps, and the active-list and step-owner telemetry only the
# CPU loop collects.
VOLATILE = re.compile(
    r"Analysis begun|Analysis ended|Total elapsed|Active Faces|dt Argmin|^\s*Backend \.\.")


def forced_deck(text, forced):
    """Return the deck with `forced` replacing those [OPTIONS] keys and a hot start saved."""
    keys = {k.upper() for k in forced}
    rows = ["%-22s %s" % item for item in forced.items()]
    out, in_options, written = [], False, False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("["):
            if in_options and not written:
                out += rows
                written = True
            in_options = stripped.upper().startswith("[OPTIONS]")
            out.append(line)
            continue
        if in_options and stripped and not stripped.startswith(";"):
            if stripped.split()[0].upper() in keys:
                continue
        out.append(line)
    if in_options and not written:
        out += rows
        written = True
    if not written:
        raise AssertionError("deck has no [OPTIONS] section")
    files = [i for i, line in enumerate(out) if line.strip().upper().startswith("[FILES]")]
    if files:
        out.insert(files[0] + 1, "SAVE HOTSTART end_state.hsf")
    else:
        out += ["", "[FILES]", "SAVE HOTSTART end_state.hsf"]
    return "\n".join(out) + "\n"


def stable_report(path):
    return [line for line in path.read_text(errors="replace").splitlines()
            if not VOLATILE.search(line)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--plugin-dir", required=True, type=Path)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--backend", default="omp")
    parser.add_argument("--threads", default=1, type=int)
    parser.add_argument("--option", action="append", default=[], metavar="KEY=VALUE",
                        help="extra [OPTIONS] override applied to both runs")
    parser.add_argument("--expect-refusal", default=None, metavar="TEXT",
                        help="the plugin run must report this unsupported item and use the CPU")
    args = parser.parse_args()
    # The engine runs inside each run folder, so every path must be absolute.
    args.engine = args.engine.resolve()
    args.plugin_dir = args.plugin_dir.resolve()
    args.input = args.input.resolve()
    args.output = args.output.resolve()

    forced = {"FV_LTS": "NO", "FV_COMPACTION": "NO", "FV_STRUCTURE_COUPLING": "ROUTING_STEP",
              "THREADS": str(args.threads)}
    for item in args.option:
        key, value = item.split("=", 1)
        forced[key] = value
    name = args.input.stem
    text = forced_deck(args.input.read_text(errors="replace"), forced)

    # Side files the deck names by relative path (rain gage data and the like).
    side = [p for p in args.input.parent.iterdir()
            if p.is_file() and p.suffix.lower() not in (".inp", ".rpt", ".out", ".h5", ".hsf")
            and p.stat().st_size < 50e6]

    # A refused run never attaches the marcher, so it keeps the CPU solver's own residual rule.
    cpu_env = {"OPENSWMM_FV_BACKEND": "cpu"}
    if args.expect_refusal is None:
        cpu_env["OPENSWMM_FV_SETTLE_SNAPSHOT"] = "1"
    logs = {}
    for tag, env, threads in (
            ("cpu", cpu_env, args.threads),
            ("device", {"OPENSWMM_FV_BACKEND": args.backend,
                        "OPENSWMM_GPU_PLUGIN_PATH": str(args.plugin_dir)}, args.threads)):
        folder = args.output / tag
        shutil.rmtree(folder, ignore_errors=True)
        folder.mkdir(parents=True)
        for p in side:
            shutil.copy(p, folder)
        deck = folder / (name + ".inp")
        deck.write_text(text)
        environment = dict(os.environ)
        environment.pop("OPENSWMM_FV_BACKEND", None)
        environment.pop("OPENSWMM_FV_SETTLE_SNAPSHOT", None)
        environment.update(env)
        environment["OMP_NUM_THREADS"] = str(threads)
        done = subprocess.run(
            [str(args.engine), str(deck), str(folder / "out.rpt"), str(folder / "out.out")],
            env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            cwd=folder)
        (folder / "console.txt").write_text(done.stdout)
        if done.returncode != 0:
            raise AssertionError(f"{tag}: engine exit {done.returncode}\n{done.stdout[-2000:]}")
        logs[tag] = done.stdout

    device_log = logs["device"]
    device_report = (args.output / "device" / "out.rpt").read_text(errors="replace")
    selected = "[openswmm FV] using device backend" in device_log
    if not selected:
        raise AssertionError("the device backend was not loaded:\n" + device_log[-2000:])

    if args.expect_refusal is not None:
        if args.expect_refusal not in device_log + device_report:
            raise AssertionError(
                f"expected a refusal naming '{args.expect_refusal}':\n{device_log[-2000:]}")
    elif "not used" in device_log or "backend not used" in device_report:
        raise AssertionError("the device backend stepped aside:\n" + device_log[-2000:])

    failures = []
    for item in ("out.out", "end_state.hsf"):
        a, b = args.output / "cpu" / item, args.output / "device" / item
        if not a.exists() or not b.exists():
            failures.append(f"{item}: missing")
        elif a.stat().st_size == 0:
            failures.append(f"{item}: empty")
        elif not filecmp.cmp(a, b, shallow=False):
            failures.append(f"{item}: differs")
    cpu_lines = stable_report(args.output / "cpu" / "out.rpt")
    dev_lines = stable_report(args.output / "device" / "out.rpt")
    if args.expect_refusal is not None:
        # The refusal itself is one extra warning line in the device report.
        dev_lines = [line for line in dev_lines if args.expect_refusal not in line]
        cpu_lines = [line for line in cpu_lines if "WARNING" not in line]
        dev_lines = [line for line in dev_lines if "WARNING" not in line]
    if cpu_lines != dev_lines:
        differing = [f"  cpu:    {a}\n  device: {b}" for a, b in zip(cpu_lines, dev_lines) if a != b]
        failures.append("report differs (%d lines, lengths %d/%d)\n%s" % (
            len(differing), len(cpu_lines), len(dev_lines), "\n".join(differing[:10])))
    if failures:
        raise AssertionError(f"{name} [{args.backend}, {args.threads} threads]:\n" +
                             "\n".join(failures))
    print(f"{name}: {args.backend} backend at {args.threads} thread(s) matches the CPU solver "
          f"(binary output, hot start, report)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
