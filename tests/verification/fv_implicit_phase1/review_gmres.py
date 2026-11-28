#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
"""Independently audit retained GMRES evidence and prior direct-path controls."""
import argparse
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT=Path(__file__).resolve().parents[3]
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture",type=Path)
    parser.add_argument("--reference",type=Path,default=ROOT/"tests/output/fv_implicit_phase1/2026-10-19_coupled_reach_final")
    args=parser.parse_args(); cap=args.capture.resolve(); previous=args.reference.resolve()
    m=json.loads((cap/"manifest.json").read_text()); assert m["status"]=="captured"
    assert m["hydraulic_profile"]=="gmres_comparator"
    for name,h in m["source_before"].items(): assert digest(cap/"source_snapshot"/name)==h,name
    for name,h in m["artifacts"].items(): assert digest(cap/"artifact_snapshot"/Path(name).name)==h,name
    spec=importlib.util.spec_from_file_location("snapshot",cap/"source_snapshot/tests/verification/fv_implicit_phase1/run.py")
    runner=importlib.util.module_from_spec(spec); spec.loader.exec_module(runner)
    prior=json.loads((previous/"manifest.json").read_text()); assert prior["status"]=="captured"
    key=lambda r:(r["case"],r["cells"],r["celerity_ft_s"],r["dt_cap_s"])
    lookup={key(r):r for r in prior["components"] if r["mode"]=="coupled"}
    result={"manifest_sha256":digest(cap/"manifest.json"),"script_sha256":digest(Path(__file__)),
        "reference_manifest_sha256":digest(previous/"manifest.json"),"source_copies":len(m["source_before"]),
        "artifact_copies":len(m["artifacts"]),"direct_controls":[],"hydraulic_audits":[],"jacobi_diagnostics":[]}
    for row in m["components"]:
        work=cap/row["run"]
        assert row["completed"] and row["max_scaled_residual"]<=1 and row["max_relative_balance"]<=1.e-10
        for name,field in (("states.csv","states_sha256"),("trace.csv","trace_sha256"),("mesh_before.csv","mesh_sha256")):
            assert digest(work/name)==row[field]
        result["hydraulic_audits"].append({"run":row["run"],**runner.audit(work,row)})
        if row["mode"]=="coupled":
            before=lookup[key(row)]
            assert digest(previous/before["run"]/"states.csv")==before["states_sha256"]
            assert before["states_sha256"]==row["states_sha256"] and before["mesh_sha256"]==row["mesh_sha256"]
            result["direct_controls"].append({"run":row["run"],"reference":before["run"],"state_and_mesh_bitwise_equal":True})
    result["linear_mode_comparisons"]=runner.compare_linear_modes(cap,m["components"])
    assert result["linear_mode_comparisons"]==m["linear_mode_comparisons"]
    for row in m["jacobi_diagnostics"]:
        work=cap/row["run"]
        for name,field in (("states.csv","states_sha256"),("trace.csv","trace_sha256"),("mesh_before.csv","mesh_sha256")):
            assert digest(work/name)==row[field]
        runner.audit(work,row)
        preserved=None
        if not row["completed"] and row["reached_s"]==0:
            with (work/"states.csv").open() as stream: values=list(csv.DictReader(stream))
            preserved=values[:row["cells"]]==values[row["cells"]:]; assert preserved
        result["jacobi_diagnostics"].append({"run":row["run"],"completed":row["completed"],"failed_initial_state_preserved":preserved})
    for name,row in m["tests"].items():
        xml=ET.parse(cap/name/"results.xml").getroot()
        assert int(xml.attrib["tests"])==row["tests"] and int(xml.attrib["failures"])==0
    result["native_test_cases"]=sum(r["tests"] for r in m["tests"].values())
    result["max_relative_balance"]=max(r["max_relative_balance"] for r in m["components"])
    result["production_promotion"]=False
    (cap/"independent_review.json").write_text(json.dumps(result,indent=2,allow_nan=False)+"\n")
    print(f"Audited {len(m['components'])} hydraulic runs, {result['native_test_cases']} tests and {len(result['direct_controls'])} prior direct controls.")


if __name__=="__main__":main()
