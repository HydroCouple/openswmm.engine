"""Require the real OpenMP plugin and compare every 2D continuity report term.

With --quality the deck also carries transport rows (a decaying pollutant with a rain
concentration, water age) and the two runs must then write the same binary output and the same
quality continuity tables.
"""

import argparse
import math
import os
from pathlib import Path
import re
import subprocess


def continuity(report):
    text = report.read_text(errors="replace")
    marker = "2D Surface Routing Continuity"
    if text.count(marker) != 1:
        raise AssertionError(f"{report}: expected one 2D continuity table")
    rows = {}
    for line in text.split(marker, 1)[1].splitlines():
        match = re.match(r"\s*(.*?)\s*\.{2,}\s+(\S+)", line)
        if not match:
            continue
        label, raw = match.groups()
        value = float(raw)
        if not math.isfinite(value) or label in rows:
            raise AssertionError(f"{report}: invalid row {line}")
        rows[label] = value
        if label == "Continuity Error (%)":
            break
    required = {
        "Initial Stored Volume", "Rainfall Inflow", "1D -> 2D Spill Inflow",
        "Outfall Inflow", "Boundary Inflow", "2D -> 1D Drain Outflow",
        "Outfall Withdrawal", "Boundary Outflow", "Evaporation Loss",
        "Infiltration Loss", "Final Stored Volume", "Continuity Error (%)",
    }
    if not required <= rows.keys():
        raise AssertionError(f"{report}: missing rows {required - rows.keys()}")
    return rows


def quality_tables(report):
    """The report's quality continuity tables, each as its lines up to the next blank line."""
    lines = report.read_text(errors="replace").splitlines()
    blocks = []
    for start, line in enumerate(lines):
        if "Quality" in line and "Continuity" in line:
            end = start
            while end < len(lines) and lines[end].strip():
                end += 1
            blocks.append("\n".join(lines[start:end]))
    if not blocks:
        raise AssertionError(f"{report}: no quality continuity table")
    return blocks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--plugin-dir", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--evaporation", action="store_true")
    parser.add_argument("--momentum", default="LOCAL_INERTIAL",
                        help="MOMENTUM_EQUATION pinned in the deck")
    parser.add_argument("--option", action="append", default=[],
                        help="extra [2D_OPTIONS] line, repeatable")
    parser.add_argument("--quality", action="store_true",
                        help="add transport rows; the binary output and the quality continuity "
                             "tables must then be identical")
    parser.add_argument("--reactions", action="store_true",
                        help="with --quality: add a two-species reaction system (RK5, FULL "
                             "coupling) whose species ride the 2D surface")
    parser.add_argument("--expect-log", metavar="TEXT",
                        help="the OpenMP run's log must contain TEXT")
    parser.add_argument("--expect-refusal", metavar="TEXT",
                        help="the plugin must be refused with a notice containing TEXT, and the "
                             "run must match the CPU run exactly because it is the CPU marcher")
    args = parser.parse_args()
    deck = args.input.read_text()
    # LOCAL_INERTIAL is no longer the default closure, so the closure is always pinned.
    extra = "".join(line + "\n" for line in args.option)
    deck, pinned = re.subn(r"(?m)^\[2D_OPTIONS\]\s*$",
                           f"[2D_OPTIONS]\nMOMENTUM_EQUATION {args.momentum}\n{extra}", deck)
    if pinned != 1:
        raise AssertionError("expected one [2D_OPTIONS] section in the input")
    if args.evaporation:
        # 100 mm/hour makes applied evaporation visible at report precision,
        # including budget-limited dry-out in the shallow cells.
        deck, count = re.subn(r"(?m)^CONSTANT\s+0\.0\s*$", "CONSTANT 2400.0", deck)
        if count != 1:
            raise AssertionError("evaporation fixture did not replace exactly one rate")
        deck, count = re.subn(r"(?m)^\[2D_OPTIONS\]\s*$",
                              "[2D_OPTIONS]\nEVAPORATION CLIMATE", deck)
        if count != 1:
            raise AssertionError("evaporation fixture requires one 2D options section")
    if args.quality:
        # A pollutant that rain carries and that decays (the host reaction stage), water age,
        # an initial concentration on the mesh and dispersion.
        deck, count = re.subn(r"(?m)^\[OPTIONS\]\s*$", "[OPTIONS]\nWATER_AGE YES", deck)
        if count != 1:
            raise AssertionError("quality fixture requires one [OPTIONS] section")
        deck, count = re.subn(r"(?m)^\[2D_OPTIONS\]\s*$", "[2D_OPTIONS]\nDISPERSION 0.5", deck)
        if count != 1:
            raise AssertionError("quality fixture requires one 2D options section")
        deck += ("\n[POLLUTANTS]\n;;Name Units Crain Cgw Crdii Kdecay\n"
                 "P1 MG/L 2.5 0 0 0.5\n"
                 "\n[2D_INITIAL_QUALITY]\n* P1 10\n")
    reactions = None
    if args.reactions:
        if not args.quality:
            raise AssertionError("--reactions requires --quality")
        # A forms at a constant rate and from the pollutant, and turns into B at a rate that
        # depends on the pollutant; B decays. The constant source keeps both nonzero on the
        # surface (the initial rows below only seed cells that start wet).
        reactions = ("[REACTION_OPTIONS]\nSOLVER RK5\nCOUPLING FULL\nRATE_UNITS HR\n"
                     "[REACTION_SPECIES]\nBULK A MG\nBULK B MG\n"
                     "[REACTION_COEFFICIENTS]\nPARAMETER k 0.3\n"
                     "[REACTION_TERMS]\nloss k * A * (1 + 0.01 * P1)\n"
                     "[REACTION_PIPES]\nRATE A 0.02 + 0.2 * P1 - loss\nRATE B loss - 0.05 * B\n"
                     "[REACTION_TANKS]\nRATE A 0.02 + 0.2 * P1 - loss\nRATE B loss - 0.05 * B\n"
                     "[REACTION_QUALITY]\nGLOBAL A 1\n")
        deck += ("\n[PROCESS_COMPONENTS]\n"
                 "org.hydrocouple.openswmm.reactions config=\"model.rxn\"\n"
                 "\n[2D_INITIAL_QUALITY]\n* A 4\n* B 0.5\n")
    results = {}
    outputs = {}
    tables = {}
    for backend in ("cpu", "omp"):
        work = (args.output / backend).resolve()
        work.mkdir(parents=True, exist_ok=True)
        inp = work / "model.inp"
        report = work / "model.rpt"
        inp.write_text(deck)
        if reactions:
            (work / "model.rxn").write_text(reactions)
        report.unlink(missing_ok=True)
        env = dict(os.environ, OPENSWMM_2D_BACKEND=backend,
                   OPENSWMM_GPU_PLUGIN_PATH=str(args.plugin_dir.resolve()),
                   OMP_NUM_THREADS="1", OMP_PROC_BIND="false", OPENSWMM_2D_HEAD_RAMP="0")
        binary = work / "model.out"
        binary.unlink(missing_ok=True)
        command = [str(args.engine.resolve()), str(inp), str(report)]
        if args.quality:
            command.append(str(binary))
        run = subprocess.run(command,
                             cwd=work, env=env, text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=180)
        (work / "run.log").write_text(run.stdout)
        if run.returncode:
            raise AssertionError(f"{backend} exited {run.returncode}: {run.stdout[-4000:]}")
        selected = "using GPU backend: omp" in run.stdout
        if backend == "omp" and args.expect_refusal:
            # A configuration outside the plugin's capabilities: it must be refused by name
            # before any plugin is selected. When the capability lands, this case becomes a
            # parity case (drop --expect-refusal) and must then select the plugin.
            if selected:
                raise AssertionError("plugin was selected for a configuration it must refuse")
            if args.expect_refusal not in run.stdout:
                raise AssertionError(f"refusal notice lacks {args.expect_refusal!r}")
        elif backend == "omp" and not selected:
            raise AssertionError("OpenMP plugin was not selected; CPU fallback cannot pass")
        if backend == "omp" and args.expect_log and args.expect_log not in run.stdout:
            raise AssertionError(f"OpenMP run log lacks {args.expect_log!r}")
        results[backend] = continuity(report)
        if args.quality:
            outputs[backend] = binary.read_bytes()
            tables[backend] = quality_tables(report)
    if args.quality:
        if outputs["cpu"] != outputs["omp"]:
            raise AssertionError("binary output differs between the CPU and OpenMP runs")
        if tables["cpu"] != tables["omp"]:
            raise AssertionError("quality continuity tables differ:\n" + "\n".join(tables["cpu"]) +
                                 "\n--- OpenMP ---\n" + "\n".join(tables["omp"]))
        print(f"binary output identical ({len(outputs['cpu'])} bytes); "
              f"{len(tables['cpu'])} quality continuity table(s) identical")
        # Where h5py is available, also require every 2D dataset but the two boundary totals
        # (summed in a different order) to hold the same bytes, and the rows to be nonzero.
        try:
            import h5py
            import numpy
        except ImportError:
            print("h5py not available: the 2D datasets were not compared")
        else:
            files = {b: h5py.File(next((args.output / b).resolve().glob("*.h5")), "r")
                     for b in ("cpu", "omp")}
            names = []
            files["cpu"].visititems(
                lambda name, item: names.append(name) if isinstance(item, h5py.Dataset) else None)
            differing = [n for n in names
                         if files["cpu"][n].dtype.kind in "fiu" and
                         numpy.asarray(files["cpu"][n]).tobytes() !=
                         numpy.asarray(files["omp"][n]).tobytes()]
            allowed = {"mass_balance_2d/boundary_in", "mass_balance_2d/boundary_out"}
            if set(differing) - allowed:
                raise AssertionError(f"2D datasets differ: {sorted(set(differing) - allowed)}")
            conc = numpy.asarray(files["omp"]["Mesh2_face_species_conc"])
            peaks = [float(numpy.nanmax(conc[:, row])) for row in range(conc.shape[1])]
            if min(peaks) <= 0.0:
                raise AssertionError(f"a transport row stayed zero on the surface: {peaks}")
            print(f"{len(names)} 2D datasets compared, {len(differing)} boundary totals differ; "
                  f"row peaks {peaks}")
    cpu, omp = results["cpu"], results["omp"]
    if cpu.keys() != omp.keys():
        raise AssertionError("CPU and plugin continuity rows differ")
    for label, reference in cpu.items():
        actual = omp[label]
        # Reports print three decimals. Allow one unit in the last printed
        # place plus a tight relative tolerance for large volumes.
        if not math.isclose(actual, reference, rel_tol=1e-7, abs_tol=0.0011):
            raise AssertionError(f"{label}: CPU={reference}, OpenMP={actual}")
        print(f"{label}: CPU={reference}, OpenMP={actual}")
    for backend, rows in results.items():
        if rows["Boundary Inflow"] <= 0 or rows["Boundary Outflow"] <= 0:
            raise AssertionError(f"{backend}: fixture must exercise both boundary directions")
        if abs(rows["Continuity Error (%)"]) > 0.001:
            raise AssertionError(f"{backend}: continuity ledger does not close")
        if args.evaporation and rows["Evaporation Loss"] <= 0:
            raise AssertionError(f"{backend}: evaporation was not exercised")


if __name__ == "__main__":
    main()
