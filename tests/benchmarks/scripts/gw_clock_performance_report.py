#!/usr/bin/env python3
"""Verify saved performance measurements and render a clearly dated status report."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import statistics

from soa_review_baseline import sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    root = args.output.resolve()
    frozen = json.loads((root / "frozen_build.json").read_text())
    assert sha(root / "engine/libopenswmm.engine.6.dylib") == frozen["frozen_library_sha256"]
    assert frozen["frozen_library_sha256"] == frozen["production_library_sha256"]
    assert sha(root / "engine/openswmm") == frozen["frozen_cli_sha256"]
    now = datetime.now(timezone.utc).isoformat()
    lines = ["# Independent groundwater clock: performance measurements", "",
             f"Status snapshot: {now}. Rerun `gw_clock_performance_report.py` to refresh.", "",
             "SHARED remains the default. Numerical acceptance is separate from these performance observations.", "",
             "All cases retain their authored event, threads, physics and report cadence. Edits select the CPU backend, "
             "local HDF5 path and clock mode. Unprofiled wall times and exclusive process times are measured separately. "
             "Each mode must have exact binary output, numerical report and HDF5 values with profiling on/off.", ""]
    verification = {"snapshot_utc": now, "frozen_build": frozen, "suites": {}}
    for suite in ("soa", "bellinge"):
        folder = root / suite
        provenance_path = folder / "provenance.json"
        if not provenance_path.exists():
            continue
        provenance = json.loads(provenance_path.read_text())
        assert provenance["engine"]["sha256"] == frozen["frozen_library_sha256"]
        assert provenance["script_sha256"] == sha(Path(__file__).with_name("gw_clock_performance.py"))
        for path, digest in (provenance["inputs"] | provenance["dependencies"]).items():
            assert sha(Path(path)) == digest
        record = folder / "results.json"
        data = json.loads(record.read_text()) if record.exists() else {"status": "running", "cases": {}}
        assert data["status"] in ("running", "complete", "failed")
        lines += [f"## {suite.upper()}: {data['status']}", ""]
        evidence = {"status": data["status"], "completed_cases": {}, "provenance_sha256": sha(provenance_path)}
        if data["status"] == "failed":
            lines += [f"Runner failure: {data.get('error', 'see log')}", ""]
        if data["cases"]:
            lines += ["| Case | SHARED / independent wall s | GW process speed ratio | Surface process change | Physical firings SHARED / independent |",
                      "|---|---:|---:|---:|---:|"]
        for name, case in data["cases"].items():
            summary = case["summary"]
            assert summary["profile_on_off_exact"]
            for pair in case["runs"]:
                for mode in ("SHARED", "INDEPENDENT"):
                    for key in ("out_sha256", "report_payload_sha256", "datasets"):
                        assert pair[mode]["plain"][key] == pair[mode]["profiled"][key]
            wall, counts = summary["median_unprofiled_wall_seconds"], summary["physical_firings"]
            lines.append(f"| {name} | {wall['SHARED']:.6f} / {wall['INDEPENDENT']:.6f} | "
                         f"{summary['gw_process_speed_ratio']:.3f}x | "
                         f"{100 * summary['surface_process_relative_change']:+.2f}% | "
                         f"{counts['SHARED']:,} / {counts['INDEPENDENT']:,} |")
            evidence["completed_cases"][name] = summary
            cell_cost = {mode: statistics.median(
                float(pair[mode]["profiled"]["phases"]["GW2D"]["cells"])
                for pair in case["runs"]) / counts[mode] for mode in ("SHARED", "INDEPENDENT")}
            evidence["completed_cases"][name]["gw_cell_kernel_seconds_per_firing"] = cell_cost
            evidence["completed_cases"][name]["gw_cell_kernel_cost_ratio"] = cell_cost["INDEPENDENT"] / cell_cost["SHARED"]
        lines.append("")
        progress_path = folder / "progress.json"
        if data["status"] == "running" and progress_path.exists():
            progress = json.loads(progress_path.read_text())
            lines += [f"Current run: {progress.get('case')} / {progress.get('mode')} / {progress.get('measurement')}.", ""]
            console = Path(progress["directory"]) / "console.log"
            if console.exists():
                matches = re.findall(r"(\d+) steps, elapsed = ([\d.]+) days", console.read_text())
                if matches:
                    steps, days = matches[-1]
                    evidence["current_progress"] = {"steps": int(steps), "simulation_days": float(days)}
                    lines += [f"Last engine progress: {int(steps):,} hydraulic steps, {float(days):.4f} simulated days. "
                              "This is partial output and is not a completed-event measurement.", ""]
        if suite == "soa":
            assert data["status"] == "complete" and len(data["cases"]) == 5
            lines += ["Three paired repeats per case. These very short runs include sub-millisecond process totals; "
                      "percent changes are observations and do not establish a reliable 2% timing bound. "
                      "The one-second producer cap makes the small SIGMA fixture perform three times as many physical GW updates. "
                      "The mixed cases reduce updates 11.57x, but each GW-cell firing costs roughly twice as much in these samples. "
                      "A process speed ratio is not a wall-time speedup.", ""]
        if suite == "bellinge":
            lines += ["Bellinge retains 172,680 simulated seconds, THREADS 8 and the original 15-minute reporting cadence. "
                      "One paired repeat is requested. A single pair still has timing uncertainty.", ""]
            if data["status"] == "complete":
                targets = data["cases"]["bellinge_full"]["summary"]["observed_performance_targets"]
                lines += ["Observed performance targets: " + "; ".join(
                    f"{key}: {'PASS' if passed else 'FAIL'}" for key, passed in targets.items()) + ".", ""]
            else:
                lines += ["The 10x GW-work and no-more-than-2% surface-regression targets remain unqualified "
                          "until all four full-event runs complete.", ""]
        verification["suites"][suite] = evidence
    amendment_path = root.parent / "node_reference_amendment/qualification.json"
    if amendment_path.exists():
        amendment = json.loads(amendment_path.read_text())
        assert amendment["accuracy_passed"] and amendment["accuracy_gates_passed"] == 18
        assert amendment["production_engine"]["sha256"] == frozen["frozen_library_sha256"]
        assert amendment["legacy_compatibility_failed"] == ["1.each_window_2pct"]
        verification["node_accuracy_amendment"] = {"qualification_sha256": sha(amendment_path), **amendment}
        lines += ["## Approved node accuracy reference", "",
                  "The user approved matched-physics SHARED as the node accuracy reference, with legacy compatibility retained. "
                  "All 18 accuracy gates pass at the explicit .125 s base; every packet matches at hydraulic windows 1/10/30 s. "
                  "The unchanged 2% limits are used. The legacy terminal-packet comparison still fails by 7.14% and remains visible "
                  "as compatibility evidence. The default one-second node base remains unqualified. SHARED remains the default.", ""]
    else:
        lines += ["## Node accuracy evidence", "",
                  "The approved node accuracy reference is matched-physics SHARED, with legacy compatibility reported separately. "
                  "No approved node qualification record was supplied in this output tree; no node-accuracy pass is inferred. "
                  "Use the explicit reference-engine option and retain reference provenance and production-INDEPENDENT equivalence. "
                  "SHARED remains the default and the one-second node base remains unqualified.", ""]
    (root / "RESULTS.md").write_text("\n".join(lines))
    (root / "verification.json").write_text(json.dumps(verification, indent=2, allow_nan=False) + "\n")
    print(f"Verified saved evidence; report: {root / 'RESULTS.md'}")


if __name__ == "__main__":
    main()
