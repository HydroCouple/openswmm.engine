#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
"""Replay provenance, original equations, full-matrix linear and cell comparisons."""
import argparse
import json
from pathlib import Path
import xml.etree.ElementTree as ET
import condensation_run as capture


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("capture",type=Path)
    args=parser.parse_args();out=args.capture.resolve();m=json.loads((out/"manifest.json").read_text())
    assert m["status"]=="captured" and m["production_promotion"] is False
    for name,expected in m["source_before"].items():assert capture.base.digest(out/"source_snapshot"/name)==expected,name
    for path,expected in m["artifacts"].items():assert capture.base.digest(out/"artifact_snapshot"/Path(path).name)==expected,path
    for name in ("condensation_run.py","condensation_review.py","network_run.py","network_review.py","run.py"):
        p=Path(__file__).resolve().parent/name
        assert capture.base.digest(p)==m["source_before"][str(p.relative_to(capture.ROOT))],p
    for target,r in m["tests"].items():
        xml=ET.parse(out/target/"results.xml").getroot()
        assert int(xml.attrib["tests"])==r["tests"] and int(xml.attrib["failures"])==0 and int(xml.attrib["errors"])==0
    for r in m["networks"]:
        work=out/r["run"]
        for name,expected in r["files"].items():assert capture.base.digest(work/name)==expected,name
        assert capture.network.audit(work,r)==r["independent"]
        assert capture.physics.face_physics(work,r)==r["face_physics"]
        assert capture.audit_linear(work,r)==r["linear_audit"]
    comparisons=capture.compare(out,m["networks"]);assert comparisons==m["comparisons"]
    for r in m["prior_full_controls"]:
        for name in ("states.csv","nodes.csv","mesh_before.csv","mesh_after.csv"):
            assert capture.base.digest(out/r["run"]/name)==capture.base.digest(Path(r["reference"])/name)
        for name in ("mesh_before.csv.closures","mesh_after.csv.closures"):
            assert (out/r["run"]/name).read_bytes()==capture.legacy_closure_members(Path(r["reference"])/name)
    result={"manifest_sha256":capture.base.digest(out/"manifest.json"),"audit_sha256":capture.base.digest(Path(__file__)),
            "networks":len(m["networks"]),"native_cases":m["test_cases"],"prior_controls":len(m["prior_full_controls"]),
            "comparisons":comparisons,"production_promotion":False}
    capture.base.write_json(out/"independent_review.json",result)
    print(f"Audited {len(m['networks'])} networks, {m['test_cases']} native cases and {len(m['prior_full_controls'])} unchanged controls.")


if __name__=="__main__":main()
