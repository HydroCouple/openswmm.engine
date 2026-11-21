"""Compare explicit 1D/2D FV worker selections with the double-precision host plugin."""
import argparse
import contextlib
import ctypes as ct
import os
from pathlib import Path
import re
import subprocess
import tempfile

import h5py
import numpy as np


def make_model(network, surface, tiers=1, momentum="FULL_SWE", terrace=False):
    fixture = Path(__file__).parent / "data/fv_device/coupled_chamber.inp"
    deck = fixture.read_text().replace("00:40:00", "00:00:12").replace("00:01:00", "00:00:01")
    deck = deck.replace("ROUTING_STEP 20", f"ROUTING_STEP 1\nFV_MIN_CELLS 8\nFV_BACKEND {network}")
    deck = deck.replace("MOMENTUM_EQUATION LOCAL_INERTIAL",
                        f"BACKEND {surface}\nLTS_TIERS {tiers}\nRECONSTRUCTION_ORDER 1\n"
                        f"MOMENTUM_EQUATION {momentum}")
    deck = deck.replace("REPORT_2D NO", "REPORT_2D YES\nOUTPUT_FILE surface.h5\n"
                        "OUTPUT_PRECISION FLOAT64\nREPORT_2D_VARIABLES DEPTH VELOCITY")
    deck = deck.replace("CONTINUITY YES", "CONTINUITY YES\nNODES ALL\nLINKS ALL")
    prefix, triangles = deck.split("[2D_TRIANGLES]")
    rows, suffix = triangles.split("[2D_VERTEX_NODE_MAP]")
    rows = [row for row in rows.splitlines() if row.strip()]
    if terrace:
        # A 1 cm film on a terrace 0.5 m above a 0.3 m deep basin. The film's cells
        # take a coarser LTS tier than the faces they drain through, so each face
        # fires several times per firing of its exporting cell.
        head, vertices = prefix.split("[2D_VERTICES]")
        points = [row.split() for row in vertices.splitlines() if row.strip()]
        prefix = head + "[2D_VERTICES]\n" + "\n".join(
            f"{x} {y} {1.5 if float(x) >= 6 else 1.0}" for x, y, _ in points) + "\n\n"
        on_terrace = [sum(float(points[int(v)][0]) for v in row.split()[:3]) / 3 > 5.5
                      for row in rows]
        wet = [row + (" 0.01" if high else " 0.3") for row, high in zip(rows, on_terrace)]
    else:
        # A wet depth discontinuity produces nonzero surface flux on the flat mesh.
        wet = [row + (" 0.12" if int(row.split()[0]) < 55 else " 0.10") for row in rows]
    return prefix + "[2D_TRIANGLES]\n" + "\n".join(wet) + "\n\n[2D_VERTEX_NODE_MAP]" + suffix


def network_fields(library, path):
    library.swmm_output_open.argtypes = [ct.c_char_p]
    library.swmm_output_open.restype = ct.c_void_p
    handle = library.swmm_output_open(os.fsencode(path))
    assert handle, path
    handle = ct.c_void_p(handle)
    try:
        periods = library.swmm_output_get_period_count(handle)
        assert periods == 12, periods
        fields = {}
        for kind, variables in (("node", ["depth"]), ("link", ["flow", "depth"])):
            count = getattr(library, f"swmm_output_get_{kind}_count")(handle)
            assert count > 0
            read = getattr(library, f"swmm_output_get_{kind}_result")
            for variable, name in enumerate(variables):
                values = (ct.c_float * count)()
                frames = []
                for period in range(periods):
                    assert read(handle, period, variable, values) == 0
                    frames.append(values[:])
                fields[f"{kind}_{name}"] = np.array(frames)
        return fields
    finally:
        library.swmm_output_close(handle)


def run(args, root, network, surface, *, receipt_trace=False, tiers=1, momentum="FULL_SWE",
        terrace=False):
    name = f"{network}-{surface}" + ("-trace" if receipt_trace else "")
    if terrace:
        name += f"-terrace-{momentum.lower()}-k{tiers}"
    directory = root / name
    directory.mkdir()
    (directory / "model.inp").write_text(make_model(network, surface, tiers, momentum, terrace))
    env = os.environ.copy()
    for key in ("OPENSWMM_2D_BACKEND", "OPENSWMM_FV_BACKEND", "OPENSWMM_2D_METAL_FACES",
                "OPENSWMM_FV_RECEIPT_TRACE"):
        env.pop(key, None)
    env.update(OPENSWMM_GPU_PLUGIN_PATH=str(args.plugin_dir), OMP_NUM_THREADS="2",
               OPENSWMM_2D_THREADS="2", OMP_PROC_BIND="false")
    if receipt_trace:
        env["OPENSWMM_FV_RECEIPT_TRACE"] = str(directory / "trace.csv")
    if args.hidden_only:
        env["ACPP_VISIBILITY_MASK"] = "omp"
    result = subprocess.run([args.cli, "model.inp", "model.rpt", "model.out"], cwd=directory,
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=120)
    if receipt_trace:
        assert 0 < result.returncode < 128, result.stdout
        assert "Metal FV network cannot advance with accepted-water tracking" in result.stdout, result.stdout
        return
    if args.hidden_only:
        assert 0 < result.returncode < 128, result.stdout
        assert "no Metal GPU available for OpenSWMM face flow" in result.stdout, result.stdout
        assert "face_worker_complete" not in result.stdout, result.stdout
        return
    assert result.returncode == 0, result.stdout
    surface_domain = "local-inertial" if momentum == "LOCAL_INERTIAL" else "surface-fv"
    expected = {domain for domain, token in (("network-fv", network), (surface_domain, surface))
                if token == "METAL"}
    started = re.findall(r"face_operator=(\S+) queue_device=(.+?) domain=(\S+)", result.stdout)
    completed = re.findall(r"face_worker_complete sweeps=([1-9]\d*) evaluations=([1-9]\d*) domain=(\S+)",
                           result.stdout)
    assert all(backend == args.operator for backend, _, _ in started), started
    assert {domain for _, _, domain in started} == expected, result.stdout
    assert {domain for _, _, domain in completed} == expected, result.stdout
    print(directory.name, started, completed)
    fields = network_fields(ct.CDLL(str(args.library)), directory / "model.out")
    with h5py.File(directory / "surface.h5") as h5:
        for name in ("time", "Mesh2_face_depth", "Mesh2_face_vx", "Mesh2_face_vy"):
            fields[name] = h5[name][:]
    assert fields["Mesh2_face_depth"].shape[0] > 1
    assert np.max(abs(fields["link_flow"])) > 0 and np.max(abs(fields["Mesh2_face_vy"])) > 0
    if terrace:
        report = (directory / "model.rpt").read_text()
        stats = re.split(r"\n\s*\n", report.split("2D Solver Statistics", 1)[1], maxsplit=1)[0]
        occupied = [int(k) for k, share in re.findall(r"LTS Tier (\d) Occupancy \(%\) \.\s+(\S+)", stats)
                    if float(share) > 0]
        assert len(occupied) > 1, f"{name}: one LTS tier only\n{stats}"
        error = re.search(r"2D Surface Routing Continuity.*?Continuity Error \(%\) \.+\s+(\S+)",
                          report, re.S)
        assert error and abs(float(error.group(1))) < 1e-3, f"{name}: 2D continuity {error and error.group(1)}"
    return fields


def assert_parity(actual, reference, label, atol=1e-5, names=None):
    np.testing.assert_array_equal(actual["time"], reference["time"])
    for name in names or reference.keys() - {"time"}:
        assert np.isfinite(actual[name]).all() and np.isfinite(reference[name]).all(), name
        # Velocity near a wet/dry interface is sensitive to division by depth.
        velocity = name in ("Mesh2_face_vx", "Mesh2_face_vy")
        np.testing.assert_allclose(actual[name], reference[name], atol=atol,
                                   rtol=5e-4 if velocity else 1e-4, err_msg=f"{label} {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("cli", "library", "plugin-dir"):
        parser.add_argument("--" + flag, required=True, type=lambda v: Path(v).resolve())
    parser.add_argument("--operator", choices=("metal", "sycl-cpu-control"), required=True)
    parser.add_argument("--hidden-only", action="store_true", help="Require a clean error with the GPU hidden")
    parser.add_argument("--work-dir", type=lambda v: Path(v).resolve(),
                        help="Keep each run's files in this new directory instead of a temporary one")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True)
    with tempfile.TemporaryDirectory() if not args.work_dir else contextlib.nullcontext(args.work_dir) as work:
        root = Path(work)
        if args.hidden_only:
            run(args, root, "METAL", "OMP")
            run(args, root, "OMP", "METAL")
            print("PASS explicit 1D and 2D Metal requests rejected the hidden GPU")
            return
        run(args, root, "METAL", "OMP", receipt_trace=True)
        reference = run(args, root, "OMP", "OMP")
        for network, surface in (("METAL", "OMP"), ("OMP", "METAL"), ("METAL", "METAL")):
            actual = run(args, root, network, surface)
            assert_parity(actual, reference, f"{network}/{surface}")
        for momentum in ("LOCAL_INERTIAL", "FULL_SWE"):
            tiered = dict(tiers=4, momentum=momentum, terrace=True)
            reference = run(args, root, "OMP", "OMP", **tiered)
            actual = run(args, root, "OMP", "METAL", **tiered)
            # The draining film is thin, so FP32 differences show above 1e-5 (at most
            # 4.3e-5 on an M1 Max, 5.9e-5 with the CPU control). Giving every firing
            # the whole budget share instead moves depths 7e-3 m and velocities 6e-2 m/s.
            # The network is left out: its weir turns erratic late in this case, and a
            # 1e-7 THETA change on the double host moves its link flows by 2e-3 m3/s.
            assert_parity(actual, reference, f"LTS_TIERS 4 {momentum}", atol=2e-4,
                          names=("Mesh2_face_depth", "Mesh2_face_vx", "Mesh2_face_vy"))
    print(f"PASS 1D, 2D and coupled FV face routing and hydraulic parity, single and multiple "
          f"LTS tiers ({args.operator})")


if __name__ == "__main__":
    main()
