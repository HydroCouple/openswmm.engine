#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
"""Independently audit captured hydraulic metrics and the zero-time limit."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    root = args.capture.resolve()
    manifest_path = root / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    assert manifest["status"] == "captured"
    for name, expected in manifest["source_before"].items():
        assert digest(root / "source_snapshot" / name) == expected, name
    for name, expected in manifest["artifacts"].items():
        assert digest(root / "artifact_snapshot" / Path(name).name) == expected, name
    results = {"capture_manifest_sha256": digest(manifest_path),
        "review_source_sha256": digest(Path(__file__)), "source_and_artifact_copies_verified": True,
        "decision": "hold_pressure_candidate_promotion", "wave_audit": [], "zero_time_limit": []}
    snapshot = root / "analysis_snapshot" / Path(__file__).name
    snapshot.parent.mkdir(exist_ok=True)
    if snapshot.exists(): assert digest(snapshot) == digest(Path(__file__))
    else: snapshot.write_bytes(Path(__file__).read_bytes())
    for row in manifest["components"]:
        work = root / row["run"]
        assert digest(work / "mesh_before.csv") == digest(work / "mesh_after.csv") == row["mesh_sha256"]
        assert digest(work / "states.csv") == row["states_sha256"]
        if row["case"] == "wave":
            # Use mesh conductance area/slot width, not the driver's speed value.
            with (work / "mesh_before.csv").open() as stream:
                section = next(r for r in csv.reader(stream) if r and r[0] == "section")
            speed = math.sqrt(32.2 * float(section[3]) / float(section[4]))
            assert abs(speed - row["effective_wave_speed_ft_s"]) < 1.e-10 * speed
            final = []
            with (work / "states.csv").open() as stream:
                for r in csv.DictReader(stream):
                    if abs(float(r["time_s"]) - row["reached_s"]) < 1.e-12:
                        final.append((float(r["x_ft"]), float(r["head_ft"])))
            assert len(final) == row["cells"]
            predicted = [.001 * math.exp(-.5 * ((x-300-speed*row["reached_s"])/60)**2) for x,_ in final]
            rms = math.sqrt(sum(((h-6)-p)**2 for (_,h),p in zip(final,predicted)) / sum(p*p for p in predicted))
            peak = max(h-6 for _,h in final) / .001
            assert abs(rms-row["wave_relative_rms"]) < 1.e-10
            assert abs(peak-row["wave_peak_ratio"]) < 1.e-10
            results["wave_audit"].append({"run":row["run"],"rms":rms,"peak_ratio":peak})
        if row["case"] == "momentum_limit":
            # For this initialized sinusoidal mode, the unscaled averaging
            # stencil attenuates amplitude by cos^2(pi/12) in one step.
            plateau = math.sin(math.pi/12)**2
            item = {k:row[k] for k in ("mode","dt_cap_s","interior_relative_q_change","filter_prediction_relative_error")}
            item["averaging_plateau_prediction"] = plateau
            item["zero_time_screen_passed"] = row["interior_relative_q_change"] <= 1.e-4 if row["dt_cap_s"] == 1.e-7 else None
            results["zero_time_limit"].append(item)
    rows = manifest["components"]
    results["component_runs"] = len(rows)
    results["component_completions"] = sum(r["completed"] for r in rows)
    results["max_relative_balance"] = max(r["max_relative_balance"] for r in rows)
    assert results["component_completions"] == len(rows)
    assert results["max_relative_balance"] <= 1.e-10
    results["source_after_equal_before"] = manifest["source_before"] == manifest["source_after"]
    results["limits"] = ["Hold is a qualification result, not an assertion that native full implicit FV is infeasible.",
        "Full E2 and UF candidate capabilities remain unsupported; production defaults were not changed.",
        "No wave threshold or momentum update was relaxed/modified to obtain acceptance."]
    (root / "independent_review.json").write_text(json.dumps(results,indent=2,allow_nan=False)+"\n")
    print(f"Audited {len(results['wave_audit'])} wave comparisons and {len(results['zero_time_limit'])} zero-time probes.")


if __name__ == "__main__": main()
