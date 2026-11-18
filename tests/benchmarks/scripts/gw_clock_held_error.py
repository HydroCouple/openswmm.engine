#!/usr/bin/env python3
"""Compare held and committed recession heads using the isolated clock-study executable."""

import argparse
import csv
import json
import os
from pathlib import Path

import numpy as np

import gw_clock_phase1 as phase1


def run_trace(
    cli,
    destination,
    case,
    mode,
    ceiling=None,
    policy="off",
    tolerance=0.05,
    capacity_clock=False,
):
    destination.mkdir()
    environment = {
        "GW_STUDY_COMMITS": "1",
        "GW_STUDY_POLICY": policy,
        "GW_STUDY_EPS": str(tolerance),
    }
    if capacity_clock:
        environment["GW_STUDY_CAPACITY"] = "1"
    data = phase1.run(
        cli,
        destination,
        case,
        mode,
        10,
        0.7,
        fixed_windows=True,
        gw_max_step=ceiling,
        environment=environment,
    )
    journal = destination / f"{case}_{mode}_10_0.7" / "commits.csv"
    if not journal.exists():
        raise ValueError(
            "Missing accepted-cell journal. Use the executable built with the held-head study patch."
        )
    columns = ("cell", "start_s", "end_s", "head_before", "head_after")
    events = np.genfromtxt(journal, delimiter=",", names=True, usecols=columns, ndmin=1)
    report_step = 600 if case == "long_recession" else 60
    times = np.arange(1, len(data["time"]) + 1) * report_step
    # The fixtures start at midnight. SWMM writes a millisecond offset into HDF5 dates.
    np.testing.assert_allclose(
        (data["time"] - np.floor(data["time"][0])) * 86400, times, rtol=0, atol=0.002
    )
    cells = []
    for i in range(data["head"].shape[1]):
        e = events[events["cell"] == i]
        assert e.size and np.isfinite(e["head_after"]).all()
        ends = np.r_[0.0, e["end_s"]]
        heads = np.r_[e["head_before"][0], e["head_after"]]
        assert np.all(np.diff(ends) > 0)
        np.testing.assert_allclose(e["start_s"], ends[:-1], rtol=0, atol=1e-7)
        np.testing.assert_array_equal(e["head_before"], heads[:-1])
        np.testing.assert_array_equal(heads[as_of(ends, times)], data["head"][:, i])
        cells.append((ends, heads))
    return times, cells


def as_of(ends, times):
    return np.searchsorted(ends, times, side="right") - 1


def write_rows(path, header, rows):
    with path.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(header.split(","))
        writer.writerows(sorted(rows))


def compare(destination, reference, independent):
    times, shared_cells = reference
    np.testing.assert_array_equal(times, independent[0])
    reports, commits = [], []
    for cell, ((st, sh), (it, ih)) in enumerate(
        zip(shared_cells, independent[1], strict=True)
    ):
        last = as_of(it, times)
        observed = as_of(st, times)
        ref_commit = as_of(st, it[1:])
        matching_time = as_of(st, it[last])
        integration = ih[last] - sh[matching_time]
        holding = sh[matching_time] - sh[observed]
        total = ih[last] - sh[observed]
        np.testing.assert_allclose(integration + holding, total, rtol=0, atol=3e-16)
        reports.extend(
            zip(
                times,
                np.full(len(times), cell),
                it[last],
                times - it[last],
                ih[last],
                sh[matching_time],
                sh[observed],
                integration,
                holding,
                total,
            )
        )
        commits.extend(
            zip(
                it[1:],
                np.full(len(ref_commit), cell),
                st[ref_commit],
                ih[1:],
                sh[ref_commit],
                ih[1:] - sh[ref_commit],
            )
        )
    write_rows(
        destination / "reports.csv",
        "report_s,cell,last_commit_s,age_s,held_head,shared_at_commit,shared_at_report,commit_error_m,hold_error_m,total_error_m",
        reports,
    )
    write_rows(
        destination / "comparisons.csv",
        "commit_s,cell,shared_commit_s,head_m,shared_head_m,delta_m",
        commits,
    )
    r, c = np.array(reports), np.array(commits)
    norm = np.linalg.norm(r[:, 6])
    result = {
        "updates": len(commits),
        "maximum_report_age_s": float(max(r[:, 3])),
        "reference_maximum_lag_at_commits_s": float(max(c[:, 0] - c[:, 2])),
        "relative_l2_at_actual_updates": phase1.relative_l2(c[:, 3], c[:, 4]),
    }
    for column, name in ((7, "commit"), (8, "hold"), (9, "total")):
        result[name] = {
            "report_relative_l2": float(np.linalg.norm(r[:, column]) / norm),
            "max_abs_m": float(max(abs(r[:, column]))),
        }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--case", choices=("dry", "long_recession"), default="long_recession"
    )
    parser.add_argument(
        "--policy",
        choices=("off", "native", "native_only", "combined", "rates", "receipts"),
        default="native",
    )
    parser.add_argument("--tolerance", type=float, default=0.05)
    parser.add_argument(
        "--capacity-clock",
        action="store_true",
        help="Use absolute infiltration-capacity updates in the targeted run",
    )
    args = parser.parse_args()
    if not 0 < args.tolerance < 1 or any(k.startswith("GW_STUDY_") for k in os.environ):
        parser.error(
            "Use a tolerance between zero and one and clear inherited GW_STUDY_ overrides."
        )
    cli, out = args.engine.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    reference = run_trace(cli, out / "shared", args.case, "SHARED")
    results = {
        "case": args.case,
        "policy": args.policy,
        "head_tolerance": args.tolerance,
        "capacity_clock": args.capacity_clock,
    }
    for label, ceiling, policy in (
        ("baseline", 3600, "off"),
        ("control_600", 600, "off"),
        ("targeted", 3600, args.policy),
    ):
        path = out / label
        sample = run_trace(
            cli,
            path,
            args.case,
            "INDEPENDENT",
            ceiling,
            policy,
            args.tolerance,
            args.capacity_clock and label == "targeted",
        )
        results[label] = compare(path, reference, sample)
        print(label, results[label], flush=True)
    (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
