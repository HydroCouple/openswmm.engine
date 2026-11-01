#!/usr/bin/env python3
"""Prepare Phase-0 large model slices without editing the user's source decks."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

from fv_perf_baseline import write_variant


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--surface", type=Path, required=True)
    p.add_argument("--bellinge", type=Path, required=True)
    p.add_argument("--twinoaks", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--repair-bellinge-pump-curve", action="store_true",
                   help="In the benchmark copy only, restore G71F12Pp1's PUMP4 curve reference")
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    provenance = {}
    def copy_variant(src, name, options, twod=""):
        dest = a.output / (name + ".inp")
        write_variant(src, dest, options)
        # Replace earlier output paths so no run can overwrite a user's HDF5.
        text = re.sub(r"^OUTPUT_FILE\s+.*$", "", dest.read_text(), flags=re.M)
        if twod:
            text += "\n[2D_OPTIONS]\nBACKEND CPU\n" + twod
        dest.write_text(text)
        provenance[name] = {"source": str(src.resolve()),
                            "source_sha256": hashlib.sha256(src.read_bytes()).hexdigest(),
                            "options": options, "2d_options": twod}
        return dest
    # 20 minutes retains the original rainfall, includes wetting activation,
    # and avoids silently replacing the large model by a dry startup test.
    for momentum in ("LOCAL_INERTIAL", "FULL_SWE"):
        for threads in (1, 4, 8):
            copy_variant(a.surface, f"soa_79202_{momentum.lower()}_t{threads}",
                         {"END_TIME": "00:20:00", "REPORT_STEP": "00:10:00", "THREADS": str(threads)},
                         f"MOMENTUM_EQUATION {momentum}\n")
    bellinge = copy_variant(a.bellinge, "soa_bellinge_5min",
                 {"END_DATE": "06/29/2012", "END_TIME": "00:06:00",
                  "REPORT_START_TIME": "00:01:00", "REPORT_STEP": "00:01:00", "THREADS": "1"},
                 "")
    if a.repair_bellinge_pump_curve:
        text = bellinge.read_text()
        if not re.search(r"^G71F12Pp1\s+PUMP4\s", text, re.M):
            p.error("Bellinge repair requires the existing G71F12Pp1 PUMP4 curve")
        if not re.search(r"^G80F66Ybas\s+STORAGE\s", text, re.M):
            p.error("Bellinge repair requires the known G80F66Ybas STORAGE curve")
        text, count = re.subn(
            r"(^G71F12Pp1\s+G71F12P\s+G71F060\s+)G80F66Ybas(\s+)",
            r"\g<1>G71F12Pp1\2", text, flags=re.M)
        if count != 1:
            p.error("Bellinge repair requires exactly one known incorrect pump reference")
        bellinge.write_text(text)
        provenance["soa_bellinge_5min"]["benchmark_repairs"] = [{
            "pump": "G71F12Pp1", "old_curve": "G80F66Ybas", "old_type": "STORAGE",
            "new_curve": "G71F12Pp1", "new_type": "PUMP4",
            "reason": "Restore the pump curve used in the original Bellinge model; no topology change",
        }]
    copy_variant(a.twinoaks, "soa_twinoaks_v2_10min", {"END_TIME": "00:10:00"})
    # Retain the two Bellinge dependencies and their hashes. Inputs are small
    # enough to keep locally; they are not downloaded or added to Git here.
    for name in ("rg_bellinge_Jun2010_Aug2021.dat", "BellingeSWMM_v021_nopervious.2dm"):
        src = a.bellinge.parent / name
        shutil.copy2(src, a.output / name)
        provenance[name] = {"source": str(src.resolve()),
                            "sha256": hashlib.sha256(src.read_bytes()).hexdigest()}
    (a.output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")


if __name__ == "__main__":
    main()
