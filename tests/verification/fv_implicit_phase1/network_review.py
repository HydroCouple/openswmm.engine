#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
"""Independently audit captured SLOT face physics, equations and provenance."""
import argparse
import csv
import json
import math
from pathlib import Path
import xml.etree.ElementTree as ET
import network_run as capture


def face_physics(work,row):
    read=lambda name:list(csv.DictReader((work/name).open()))
    with (work/"mesh_before.csv").open() as stream: raw=list(csv.reader(stream))
    mesh={r[0]:[float(v) for v in r[1:]] for r in raw if r[0]!="section"}
    sections={int(r[1]):[float(v) for v in r[2:]] for r in raw if r[0]=="section"}
    states=read("states.csv"); nodes=read("nodes.csv"); faces=read("faces.csv")
    n=row["cells"]; nf=len(mesh["face_cl"]); nn=len(mesh["node_kind"])
    largest={"area_ft2":0.,"mass_cfs":0.,"momentum":0.,"correction":0.}
    def closure(g,h):
        # All captured states are above the taper. Independent analytic slot
        # extension of the retained section constants, no engine callbacks.
        yfull,ac,t,ic=g[1:5]; d=h-yfull
        assert d>=.00999999999
        return ac+t*d,ic+ac*d+.5*t*d*d
    for step in range(1,row["steps"]+1):
        cells=states[step*n:(step+1)*n]; nd=nodes[step*nn:(step+1)*nn]
        for i,s in enumerate(cells):
            area,_=closure(sections[int(mesh["cell_geom"][i])],float(s["depth_ft"]))
            err=abs(area-float(s["area_ft2"])); largest["area_ft2"]=max(largest["area_ft2"],err)
            assert err<=8*math.ulp(area)
        for f in range(nf):
            geometry=sections[int(mesh["face_geom"][f])]
            incident=[int(mesh["face_cl"][f]),int(mesh["face_cr"][f])]
            eta=[0.,0.]; bed=[0.,0.]; velocity=[0.,0.]; raw_i1=[0.,0.]
            for side,c in enumerate(incident):
                if c<0: continue
                s=cells[c]; bed[side]=mesh["cell_zb"][c]
                eta[side]=bed[side]+float(s["depth_ft"])
                velocity[side]=mesh["face_dir_l" if side==0 else "face_dir_r"][f]*float(s["discharge_cfs"])/float(s["area_ft2"])
                _,raw_i1[side]=closure(sections[int(mesh["cell_geom"][c])],float(s["depth_ft"]))
            for side,c in enumerate(incident):
                if c>=0: continue
                other=1-side; j=int(mesh["face_node"][f]); bed[side]=mesh["face_zb"][f]
                eta[side]=float(nd[j]["head_ft"]) if j>=0 else eta[other]
                velocity[side]=velocity[other] if j>=0 else -velocity[other]
            zstar=max(bed); a=[]; i1=[]; q=[]; c=[]; physical=[]
            for side in range(2):
                area,moment=closure(geometry,eta[side]-zstar)
                a.append(area); i1.append(moment); q.append(area*velocity[side]); c.append(math.sqrt(32.2*area/geometry[3]))
                physical.append(q[side]*velocity[side]+32.2*moment)
            sl=min(velocity[0]-c[0],velocity[1]-c[1]); sr=max(velocity[0]+c[0],velocity[1]+c[1])
            if sl>=0: mass,momentum=q[0],physical[0]
            elif sr<=0: mass,momentum=q[1],physical[1]
            else:
                mass=(sr*q[0]-sl*q[1]+sl*sr*(a[1]-a[0]))/(sr-sl)
                momentum=(sr*physical[0]-sl*physical[1]+sl*sr*(q[1]-q[0]))/(sr-sl)
            receipt=faces[(step-1)*nf+f]
            for key,predicted,field in (("mass_cfs",mass,"mass_cfs"),("momentum",momentum,"momentum")):
                actual=float(receipt[field]); error=abs(predicted-actual); largest[key]=max(largest[key],error)
                assert error<=64*math.ulp(max(1.,abs(predicted))), (work,f,key,error)
            for side in range(2):
                correction=32.2*(raw_i1[side]-i1[side]) if incident[side]>=0 else 0.
                actual=float(receipt["correction_left" if side==0 else "correction_right"])
                error=abs(correction-actual); largest["correction"]=max(largest["correction"],error)
                assert error<=64*math.ulp(max(1.,abs(correction))), (work,f,side,error)
    return largest


def main():
    parser=argparse.ArgumentParser(description=__doc__); parser.add_argument("capture",type=Path)
    args=parser.parse_args(); out=args.capture.resolve(); m=json.loads((out/"manifest.json").read_text())
    assert m["status"]=="captured" and m["production_promotion"] is False
    for name,expected in m["source_before"].items(): assert capture.base.digest(out/"source_snapshot"/name)==expected,name
    for path,expected in m["artifacts"].items(): assert capture.base.digest(out/"artifact_snapshot"/Path(path).name)==expected,path
    # Verify the audit and its imported helper code are exactly the captured
    # versions before calling their pure data checks.
    for name in ("network_review.py","network_run.py","run.py"):
        path=Path(__file__).resolve().parent/name
        assert capture.base.digest(path)==m["source_before"][str(path.relative_to(capture.ROOT))],path
    for target,expected in m["tests"].items():
        xml=ET.parse(out/target/"results.xml").getroot()
        assert int(xml.attrib["tests"])==expected["tests"] and int(xml.attrib["failures"])==0 and int(xml.attrib["errors"])==0
    results=[]
    for row in m["networks"]:
        work=out/row["run"]
        for name,expected in row["files"].items(): assert capture.base.digest(work/name)==expected
        ledger=capture.audit(work,row); assert ledger==row["independent"]
        results.append({"run":row["run"],"ledger":ledger,"face_physics":face_physics(work,row)})
    for row in m["reach_controls"]:
        work=out/row["run"]; reference=Path(row["reference"])
        for name in ("states.csv","mesh_before.csv","mesh_after.csv"):
            assert capture.base.digest(work/name)==capture.base.digest(reference/name)
        assert capture.base.digest(work/"states.csv")==row["states_sha256"]
    restart_comparisons=[]
    for row in m["networks"]:
        if row["restart"]==40: continue
        ref=next(v for v in m["networks"] if v["case"]==row["case"] and v["restart"]==40 and
                 v["cells_per_conduit"]==row["cells_per_conduit"] and v["celerity_ft_s"]==row["celerity_ft_s"] and v["moving"])
        def final(v):
            with (out/v["run"]/"states.csv").open() as stream: return list(csv.DictReader(stream))[-v["cells"]:]
        a,b=final(row),final(ref)
        errors={field:max(abs(float(x[field])-float(y[field])) for x,y in zip(a,b)) for field in ("depth_ft","area_ft2","discharge_cfs")}
        assert errors["depth_ft"]<=1.e-8 and errors["area_ft2"]<=1.e-10 and errors["discharge_cfs"]<=1.e-8
        restart_comparisons.append({"run":row["run"],"reference":ref["run"],"errors":errors})
    result={"manifest_sha256":capture.base.digest(out/"manifest.json"),"audit_sha256":capture.base.digest(Path(__file__)),
            "native_cases":m["test_cases"],"network_runs":len(results),"reach_controls":len(m["reach_controls"]),
            "production_promotion":False,"results":results,"restart_comparisons":restart_comparisons}
    capture.base.write_json(out/"independent_review.json",result)
    print(f"Audited {len(results)} networks, {m['test_cases']} native cases and {len(m['reach_controls'])} unchanged reach controls.")


if __name__=="__main__": main()
