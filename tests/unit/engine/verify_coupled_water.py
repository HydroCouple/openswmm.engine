# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
"""Exact rational audit of observed native phases; no alternate solver or tolerance."""
from fractions import Fraction as F
from pathlib import Path
import argparse
import json
import os
import subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--engine',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
p=args.output.resolve()
p.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,OPENSWMM_2D_BACKEND='cpu',OPENSWMM_COUPLED_WATER_EVIDENCE=str(p/'native-fixtures'))
with (p/'native-water-fixtures.log').open('w') as log:
    subprocess.run([str(args.engine.resolve()),'--gtest_filter=CoupledWaterCheckpoint.Authored*',
                    '--gtest_output=xml:'+str(p/'native-water-fixtures.xml')],
                   env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
records=[]
geometry=[]
counts=dict(routing_steps=0,point_bookings=0,point_conversion_roundings=0,node_addition_roundings=0,
            genuine_half_deliveries=0,whole_remainders=0,changing_rate_memory_steps=0,empty_queue_tail_steps=0)
for sign,step_size,step_count in [(sign,F(1,1024),12) for sign in ['positive','negative']] + [(sign,F(1,8),7) for sign in ['tail-positive','tail-negative']]:
    path=p/'native-fixtures'/f'water-{sign}-water.txt'
    assert path.read_bytes()==(path.parent/f'water-{sign}-plain-water.txt').read_bytes()
    headers=path.read_text().splitlines()[0].split()[1:]
    steps={}
    for line in path.read_text().splitlines()[1:]:
        values=line.split(); row=dict(zip(headers,[int(values[0])]+[float.fromhex(x) for x in values[1:]]))
        steps.setdefault(row['start'],{})[row['phase']]=row
    consumed=F(0); integrated=F(0)
    for i,phases in enumerate(steps.values(),1):
        before,assembled,routed,advanced=[phases[k] for k in [10,40,50,60]]
        dt=F(before['end'])-F(before['start']); assert dt==step_size
        queue,span=F(before['queue']),F(before['span'])
        remaining=F(assembled['queue']); q=F(assembled['coupling_rate'])
        assert F(assembled['span'])==max(F(0),span-dt)
        if span>dt:
            assert span==2*dt and remaining==queue/2 and q*dt==queue/2 and queue!=0
            counts['genuine_half_deliveries']+=1
        elif queue:
            assert remaining==0 and q*dt==queue
            counts['whole_remainders']+=1
        else:
            assert q==0 and remaining==0
        assert assembled['coupling_volume']==0
        assert q==F(routed['inflow'])-F(routed['outflow'])
        oldq=F(before['inflow'])-F(before['outflow'])
        assert F(routed['old_net'])==oldq
        vold=F(routed['old_volume']); vnew=F(routed['volume'])
        assert vold==F(before['volume']) and vnew>F(routed['full_volume'])
        trapezoid=dt*(oldq+q)/2
        native_dv=F((float(oldq+q)*.5)*float(dt))
        assert float(vold+native_dv)==routed['volume']
        area=F(routed['pond_area']); old_depth=F(routed['old_depth']); depth=F(routed['depth'])
        # The isolated ponded native path separately rounds the depth increment
        # and final addition. Check those actual observed operations, not a
        # constant geometric-error tolerance from the fixed-pool receiver.
        assert old_depth==F(before['depth'])
        assert all(routed[k]==before[k] for k in ['full_volume','full_depth','pond_area'])
        dh=float(native_dv/area)
        assert routed['depth']==float(old_depth+F(dh)), (sign,i,'depth update')
        full=F(routed['full_volume']); full_depth=F(routed['full_depth'])
        old_residual=vold-(full+(old_depth-full_depth)*area)
        residual=vnew-(full+(depth-full_depth)*area)
        volume_rounding=vnew-vold-native_dv
        depth_rounding=depth-old_depth-native_dv/area
        assert residual==old_residual+volume_rounding-area*depth_rounding
        geometry.append(dict(sign=sign,step=i,exact_geometric_residual=str(residual),
            residual_native=float(residual),native_dv_hex=float(native_dv).hex(),trapezoid_rounding=str(native_dv-trapezoid),
            volume_rounding=str(volume_rounding),depth_rounding=str(depth_rounding)))
        if q!=oldq and oldq:
            assert routed['volume']!=float(vold+dt*q/2)
            counts['changing_rate_memory_steps']+=1
        consumed+=queue-remaining; integrated+=trapezoid
        # For fixed dyadic dt the consumed queue and integrated hydraulics have
        # this endpoint difference before final storage rounding is applied.
        assert consumed-integrated==dt*q/2
        counts['routing_steps']+=1
        if sign.startswith('tail-'):
            if i>=4:
                assert advanced['queue']==0 and advanced['span']==0
                assert all(advanced[f'point{k}']==0 for k in range(6))
            if i==4:
                assert q!=0 and oldq==q and advanced['coupling_rate']==float(q)
            if i==5:
                assert q==0 and oldq!=0 and remaining==queue==0 and vnew!=vold
                assert consumed==integrated
                counts['empty_queue_tail_steps']+=1
            if i>=6: assert q==oldq==0 and vnew==vold
        if advanced['span']>assembled['span']:
            points=[advanced[f'point{k}'] for k in range(6)]
            assert all((v>0)==(k%2==0) for k,v in enumerate(points))
            factor=F(advanced['si_to_native']); running=0.; exact=F(0)
            conversion_error=F(0); accumulation_error=F(0)
            for point in points:
                product=F(point)*factor; converted=float(product)
                conversion_error+=F(converted)-product
                counts['point_conversion_roundings']+=F(converted)!=product
                addition=F(running)+F(converted); next_sum=float(addition)
                accumulation_error+=F(next_sum)-addition
                counts['node_addition_roundings']+=F(next_sum)!=addition
                running=next_sum; exact+=F(converted); counts['point_bookings']+=1
            assert advanced['queue']==float(F(remaining)+F(running))
            assert F(running)-exact==accumulation_error
            assert advanced['span']==2*float(dt)
            records.append(dict(sign=sign,step=i,booked_native_hex=running.hex(),
                exact_converted_point_sum=str(exact),conversion_error=str(conversion_error),
                node_addition_error=str(accumulation_error),
                surface_change=str(F(advanced['surface_volume'])-F(routed['surface_volume']))))
    assert len(steps)==step_count
assert counts['genuine_half_deliveries']==12 and counts['whole_remainders']==12
assert counts['empty_queue_tail_steps']==2
summary={'scope':'Observed actual native water-only engine phases. Raw points are per-window SI sums; this is not a per-firing native-leg proof. No material-receiver eligibility change.',
         'counts':counts,'point_windows':records,'geometry_roundoff':geometry,
         'hydraulic_rule':'Storage advances with previous/current net-flow trapezoid; delivery queue uses current rate. Endpoint difference is dt*q/2 before trapezoid and storage rounding in these constant-step runs; their roundoff is separately recorded.',
         'limitation':'Both directions are active in each booked window. Two authored near-equilibrium cases reach the existing ORIFICE cutoff and recover an idle-boundary zero-queue hydraulic tail. Global exact conservation and changing-volume pollutant delivery are not claimed.'}
(p/'native-water-audit.json').write_text(json.dumps(summary,indent=2)+'\n')
print(counts)
