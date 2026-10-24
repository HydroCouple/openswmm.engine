#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
"""Independent bounded physical-storage and original-cell receipt audit."""
import csv
import math
import network_run as previous


def audit(work,row):
    read=lambda name:list(csv.DictReader((work/name).open()))
    for suffix in ("", ".closures"):
        assert previous.base.digest(work/("mesh_before.csv"+suffix))==previous.base.digest(work/("mesh_after.csv"+suffix))
    with (work/"mesh_before.csv").open() as stream: raw=list(csv.reader(stream))
    mesh={r[0]:[float(v) for v in r[1:]] for r in raw if r[0]!="section"}
    sections={int(r[1]):[float(v) for v in r[2:]] for r in raw if r[0]=="section"}
    states,nodes,faces,trace,stored=(read(name+".csv") for name in ("states","nodes","faces","trace","storage"))
    sources=read("sources.csv"); n=row["cells"]; nn=len(mesh["node_kind"]); nf=len(mesh["face_cl"])
    storage_nodes=[j for j in range(nn) if mesh["node_kind"][j]==2]; ns=len(storage_nodes)
    assert len(states)==n*(row["steps"]+1) and len(nodes)==nn*(row["steps"]+1)
    assert len(faces)==nf*row["steps"] and len(stored)==ns*(row["steps"]+1) and len(trace)==row["steps"]
    assert all(math.isfinite(float(v)) for data in (states,nodes,faces,trace,stored) for r in data for v in r.values())
    cell_source=[0.]*n; node_source=[0.]*nn
    for r in sources: (cell_source if r["kind"]=="cell" else node_source)[int(r["index"])]=float(r["rate_cfs"])
    def volume(j,head):
        depth=head-mesh["node_invert"][j]; top=mesh["node_vol_dmax"][j]
        assert 0<=depth<=min(top,mesh["node_full_depth"][j])
        start=int(mesh["node_vol_off"][j]); table=mesh["node_vol_tbl"][start:start+129]
        assert len(table)==129 and table[0]==0 and all(b>a for a,b in zip(table,table[1:]))
        # Express interpolation as a convex combination, independently of the
        # engine's segment slope expression (roundoff accounted for below).
        coordinate=depth*128/top; i=min(int(coordinate),127); weight=coordinate-i
        return (1-weight)*table[i]+weight*table[i+1]
    inventories=[]; worst_curve=0.; knot_crossings=0
    for step in range(row["steps"]+1):
        total=math.fsum(float(r["dx_ft"])*float(r["area_ft2"]) for r in states[step*n:(step+1)*n])
        volumes=[]
        for r in stored[step*ns:(step+1)*ns]:
            j=int(r["node"]); assert j in storage_nodes
            head=float(nodes[step*nn+j]["head_ft"]); predicted=volume(j,head); actual=float(r["volume_ft3"])
            error=abs(predicted-actual); worst_curve=max(worst_curve,error)
            assert error<=4*math.ulp(max(1.,actual))
            assert float(r["lateral_cfs"])==node_source[j]; volumes.append(predicted)
            if step:
                oldhead=float(nodes[(step-1)*nn+j]["head_ft"])
                knot_crossings+=int((oldhead-mesh["node_invert"][j])*128/mesh["node_vol_dmax"][j])!=int((head-mesh["node_invert"][j])*128/mesh["node_vol_dmax"][j])
        inventories.append(total+math.fsum(volumes))
    external=source=0.; largest={"cell_volume_ft3":0.,"storage_volume_ft3":0.,"momentum_ft4_s":0.,"junction_cfs":0.,"relative_balance":0.}
    for step,t in enumerate(trace,1):
        old=states[(step-1)*n:step*n]; new=states[step*n:(step+1)*n]; fs=faces[(step-1)*nf:step*nf]
        dt=float(t["dt_s"]); mass=[float(f["mass_cfs"]) for f in fs]; incoming=node_source.copy(); out=0.
        for f,q in enumerate(mass):
            j=int(mesh["face_node"][f])
            if j>=0:
                flow=(1 if mesh["face_cl"][f]>=0 else -1)*q
                if int(nodes[step*nn+j]["fixed"]): out+=flow
                else: incoming[j]+=flow
        for i,(a,b) in enumerate(zip(old,new)):
            outgoing=force=0.
            for facekey,sidekey in (("cell_face0","cell_side0"),("cell_face1","cell_side1")):
                f=int(mesh[facekey][i]); side=int(mesh[sidekey][i]); sign=1 if side==0 else -1
                outgoing+=sign*mass[f]
                force+=sign*mesh["face_dir_l" if side==0 else "face_dir_r"][f]*(float(fs[f]["momentum"])+float(fs[f]["correction_left" if side==0 else "correction_right"]))
            dx=float(b["dx_ft"]); area=float(b["area_ft2"]); q=float(b["discharge_cfs"])
            rv=dx*(area-float(a["area_ft2"]))+dt*(outgoing-cell_source[i])
            budget=1.e-11+1.e-12*max(1.,dx*float(a["area_ft2"]))
            assert abs(rv)<=budget*1.001,(work,i,rv,budget)
            radius=sections[int(mesh["cell_geom"][i])][-1]
            friction=mesh["conduit_rough_factor"][int(mesh["cell_conduit"][i])]*abs(q/area)*q/(radius*math.cbrt(radius))
            rq=dx*(q-float(a["discharge_cfs"]))+dt*(force+dx*friction)
            assert abs(rq)<=(1.e-11+1.e-12*max(1.,dx*abs(float(a["discharge_cfs"]))))*1.001
            largest["cell_volume_ft3"]=max(largest["cell_volume_ft3"],abs(rv)); largest["momentum_ft4_s"]=max(largest["momentum_ft4_s"],abs(rq))
        for j in range(nn):
            if mesh["node_kind"][j]==2:
                va=volume(j,float(nodes[(step-1)*nn+j]["head_ft"])); vb=volume(j,float(nodes[step*nn+j]["head_ft"]))
                r=vb-va-dt*incoming[j]; budget=1.e-11+1.e-12*max(1.,va)
                assert abs(r)<=budget*1.001,(work,j,r,budget)
                logged=stored[step*ns+storage_nodes.index(j)]
                assert abs(r-float(logged["residual_ft3"]))<=8*math.ulp(max(1.,va))
                largest["storage_volume_ft3"]=max(largest["storage_volume_ft3"],abs(r))
            elif mesh["node_kind"][j]==0:
                largest["junction_cfs"]=max(largest["junction_cfs"],abs(incoming[j])); assert abs(incoming[j])<1.e-10
        external+=dt*out; source+=dt*(math.fsum(cell_source)+math.fsum(node_source))
        balance=inventories[step]-inventories[0]+external-source
        largest["relative_balance"]=max(largest["relative_balance"],abs(balance)/inventories[0])
        assert abs(float(t["balance_ft3"])-balance)<1.e-9
        assert abs(float(t["source_ft3"])-source)<1.e-12 and abs(float(t["external_out_ft3"])-external)<1.e-10
        assert float(t["scaled_residual"])<=1
    assert largest["relative_balance"]<1.e-10
    if row["moving"]: assert knot_crossings>0
    return dict(largest,curve_roundoff_ft3=worst_curve,knot_crossings=knot_crossings)


def linear(work,row):
    lines=list(csv.DictReader((work/"linear.csv").open())); assert len(lines)==row["newton"]
    reuse=row["mode"].endswith("_reuse"); totals=0; ratio=0.
    for r in lines:
        assert int(r["linear_converged"])==int(r["accepted_step"])==1
        assert float(r["full_true_residual"])<=float(r["full_tolerance"])
        ratio=max(ratio,float(r["full_true_residual"])/float(r["full_tolerance"]))
        assert int(r["buffer_bound_bytes"])<=64*1024*1024
        assert int(r["workspace_enabled"])==reuse
        assert int(r["retained_workspace_bytes"])<=int(r["buffer_bound_bytes"])
        if not row["mode"].startswith(("full","fallback")):
            assert int(r["condensation_status"])==1 and int(r["fallback"])==0
        if row["mode"].startswith("fallback"): assert int(r["fallback"])==1
        totals+=int(r["gmres_iterations"])+(int(r["reduced_iterations"]) if int(r["fallback"]) else 0)
    assert totals==row["gmres"]
    return {"corrections":len(lines),"maximum_original_residual_ratio":ratio,
            "symbolic_hits":sum(int(r["symbolic_reused"]) for r in lines)}
