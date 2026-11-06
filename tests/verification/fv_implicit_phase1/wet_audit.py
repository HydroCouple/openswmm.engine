#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
"""Independent bounded wet SLOT closure, face and conservation audit."""
import csv
import math
import struct
import network_run as previous


def read_closures(work):
    data=(work/"mesh_before.csv.closures").read_bytes(); size=12*8+1+4*129*8+129*2
    assert len(data)%size==0; result=[]
    for offset in range(0,len(data),size):
        scalars=struct.unpack_from("=12d",data,offset); kind=data[offset+96]
        arrays=struct.unpack_from("=516d",data,offset+97)
        result.append((scalars,kind,[arrays[i*129:(i+1)*129] for i in range(4)]))
    return result


def closure(c,h):
    s,kind,tables=c; full,dh,inv,ac,slot,ic,rfull=s[:7]
    assert h>=.00999999999
    if h>=full:
        d=h-full; return ac+slot*d,slot,ic+ac*d+.5*slot*d*d,rfull
    if kind==1:
        c2,c1,p0,p1=s[8:12]; a=(c2*h+c1)*h
        return a,2*c2*h+c1,(c2*h/3+.5*c1)*h*h,a/(p0+p1*h)
    i=min(max(int(h*inv),0),127); t=min(max(h*inv-i,0.),1.)
    aa,mm,ii,rr=tables; m0=mm[i]*dh; m1=mm[i+1]*dh
    # Power-basis evaluation independent of the engine's Hermite-basis sums.
    a0=aa[i]; a1=m0; a2=-3*aa[i]-2*m0+3*aa[i+1]-m1; a3=2*aa[i]+m0-2*aa[i+1]+m1
    a=a0+t*(a1+t*(a2+t*a3)); width=(a1+t*(2*a2+3*t*a3))*inv
    moment=ii[i]+dh*t*(a0+t*(a1/2+t*(a2/3+t*a3/4)))
    return a,width,moment,rr[i]+t*(rr[i+1]-rr[i])


def face_physics(work,row):
    read=lambda name:list(csv.DictReader((work/name).open()))
    with (work/"mesh_before.csv").open() as stream: raw=list(csv.reader(stream))
    mesh={r[0]:[float(v) for v in r[1:]] for r in raw if r[0]!="section"}
    sections={int(r[1]):[float(v) for v in r[2:]] for r in raw if r[0]=="section"}
    closures=read_closures(work); states=read("states.csv"); nodes=read("nodes.csv"); faces=read("faces.csv")
    n=row["cells"]; nn=len(mesh["node_kind"]); nf=len(mesh["face_cl"])
    largest={"area_ft2":0.,"mass_cfs":0.,"momentum":0.,"correction":0.}
    regimes={"free_sides":0,"slot_sides":0,"mixed_faces":0,"vented_sides":0,"crown_crossings":0}
    for step in range(1,row["steps"]+1):
        cells=states[step*n:(step+1)*n]; nd=nodes[step*nn:(step+1)*nn]
        for i,r in enumerate(cells):
            g=int(mesh["cell_geom"][i]); predicted=closure(closures[g],float(r["depth_ft"]))[0]
            error=abs(predicted-float(r["area_ft2"])); largest["area_ft2"]=max(largest["area_ft2"],error)
            assert error<=32*math.ulp(max(1.,predicted))
            old=float(states[(step-1)*n+i]["depth_ft"]); new=float(r["depth_ft"]); crown=sections[g][1]
            regimes["crown_crossings"]+=int((old<crown)!=(new<crown))
        for f in range(nf):
            g=int(mesh["face_geom"][f]); ids=[int(mesh["face_cl"][f]),int(mesh["face_cr"][f])]
            eta=[0.,0.]; bed=[0.,0.]; u=[0.,0.]; raw=[0.,0.]
            for side,i in enumerate(ids):
                if i<0: continue
                h=float(cells[i]["depth_ft"]); bed[side]=mesh["cell_zb"][i]; eta[side]=bed[side]+h
                u[side]=mesh["face_dir_l" if side==0 else "face_dir_r"][f]*float(cells[i]["discharge_cfs"])/float(cells[i]["area_ft2"])
                raw[side]=closure(closures[int(mesh["cell_geom"][i])],h)[2]
            j=int(mesh["face_node"][f])
            for side,i in enumerate(ids):
                if i>=0: continue
                bed[side]=mesh["face_zb"][f]; eta[side]=float(nd[j]["head_ft"]) if j>=0 else eta[1-side]
                u[side]=u[1-side] if j>=0 else -u[1-side]
            a=[]; q=[]; c=[]; moment=[]; press=[]; momentum=[]
            for side in range(2):
                h=eta[side]-max(bed); area,width,i1,_=closure(closures[g],h)
                a.append(area); q.append(area*u[side]); c.append(math.sqrt(32.2*area/width)); moment.append(i1)
                press.append(2 if ids[side]<0 and j>=0 and mesh["node_sur_depth"][j]<=0 else int(h>=sections[g][1]))
                regimes["free_sides" if h<sections[g][1] else "slot_sides"]+=1
                regimes["vented_sides"]+=int(press[-1]==2); momentum.append(q[-1]*u[side]+32.2*i1)
            sl=min(u[0]-c[0],u[1]-c[1]); sr=max(u[0]+c[0],u[1]+c[1])
            if press[0]!=press[1] and max(press)<2:
                regimes["mixed_faces"]+=1
                if press[0]:
                    da=a[0]-a[1]; w=(q[0]-q[1])/da if da>0 else sr
                    sr=max(u[1]+c[1],min(sr,w))
                else:
                    da=a[1]-a[0]; w=(q[1]-q[0])/da if da>0 else sl
                    sl=min(u[0]-c[0],max(sl,w))
            if sl>=0: mass,force=q[0],momentum[0]
            elif sr<=0: mass,force=q[1],momentum[1]
            else:
                mass=(sr*q[0]-sl*q[1]+sl*sr*(a[1]-a[0]))/(sr-sl)
                force=(sr*momentum[0]-sl*momentum[1]+sl*sr*(q[1]-q[0]))/(sr-sl)
            r=faces[(step-1)*nf+f]
            # Equivalent polynomial evaluation changes cancellation through the
            # stiff crown. Bound the resulting roundoff in physical flux units.
            for key,predicted,actual,tol in (("mass_cfs",mass,float(r["mass_cfs"]),2.e-9),
                ("momentum",force,float(r["momentum"]),2.e-8)):
                error=abs(predicted-actual); largest[key]=max(largest[key],error)
                assert error<=tol*max(1.,abs(predicted)),(work,f,key,error)
            for side in range(2):
                predicted=32.2*(raw[side]-moment[side]) if ids[side]>=0 else 0.
                actual=float(r["correction_left" if side==0 else "correction_right"])
                error=abs(predicted-actual); largest["correction"]=max(largest["correction"],error)
                assert error<=2.e-10*max(1.,abs(predicted))
    return dict(largest,**regimes)


def audit(work,row):
    read=lambda name:list(csv.DictReader((work/name).open()))
    for suffix in ("", ".closures"):
        assert previous.base.digest(work/("mesh_before.csv"+suffix))==previous.base.digest(work/("mesh_after.csv"+suffix))
    with (work/"mesh_before.csv").open() as stream: raw=list(csv.reader(stream))
    mesh={r[0]:[float(v) for v in r[1:]] for r in raw if r[0]!="section"}
    sections={int(r[1]):[float(v) for v in r[2:]] for r in raw if r[0]=="section"}
    closures=read_closures(work)
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
            radius=closure(closures[int(mesh["cell_geom"][i])],float(b["depth_ft"]))[3]
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
