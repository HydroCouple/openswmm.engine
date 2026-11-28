"""Inspect Rubinato physical-shaft temporal sensitivity with a supplied engine library.

Each simulation runs in a separate process. No coefficient or geometry fitting.
All paths, decks, series, reports, errors and hashes are retained for review.
"""
import argparse
import concurrent.futures
import csv
import ctypes as ct
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import time

import numpy as np

ROOT = Path(__file__).resolve().parents[3]
CASES = [f'{regime}_{i}_SHAFT' for regime, count in [('S1', 15), ('S2', 5), ('S3', 8), ('S4', 8)]
         for i in range(1, count+1)]
DIAG_NAMES = ['evaluations', 'nonzero_evaluations', 'clipped_evaluations',
              'sum_abs_raw_q', 'sum_abs_capped_q', 'sum_abs_suppressed_q',
              'window_min_s', 'window_max_s', 'cap_min_m3s', 'cap_max_m3s',
              'abs_dh_min_m', 'abs_dh_max_m', 'head1_min_m', 'head1_max_m',
              'head2_min_m', 'head2_max_m', 'area_min_m2', 'area_max_m2',
              'budget_nonzero_evaluations', 'budget_clipped_evaluations', 'budget_exhausted_evaluations',
              'sum_abs_before_budget_q', 'sum_abs_after_budget_q', 'substep_min_s', 'substep_max_s']


class Stats(ct.Structure):
    _fields_ = [('backend', ct.c_char*64), ('momentum', ct.c_int), ('lts_tiers', ct.c_int),
                ('steps', ct.c_long), ('face_evals', ct.c_long), ('last_step', ct.c_double),
                ('active_frac_min', ct.c_double), ('active_frac_mean', ct.c_double),
                ('active_frac_max', ct.c_double), ('n_tiers', ct.c_int),
                ('tier_cells', ct.c_long*8)]


def deck(job):
    path = Path(job['input_directory'])/f"{job['case']}.inp"
    text = path.read_text()
    text = re.sub(r'^ROUTING_STEP .*$', f"ROUTING_STEP {job['step']}", text, flags=re.M)
    options = ['VARIABLE_STEP 0', 'LENGTHENING_STEP 0']
    if job['setting'] != 'default':
        options.append('MAX_TRIALS 80')
    if job['setting'] in ['tight4', 'tight5']:
        options.append('HEAD_TOLERANCE '+('0.0001' if job['setting']=='tight4' else '0.00001'))
    text = text.replace('\nMIN_SURFAREA ', '\n'+'\n'.join(options)+'\nMIN_SURFAREA ', 1)
    if job.get('reference_step', 0.0) > 0.0:
        text = text.replace('MAX_TIMESTEP 1', f"MAX_TIMESTEP 1\nTHETA_REFERENCE_STEP {job['reference_step']}")
    if job.get('theta') is not None:
        text = text.replace('MAX_TIMESTEP 1', f"MAX_TIMESTEP 1\nTHETA {job['theta']}")
    if job.get('lts_tiers'):
        text = text.replace('MAX_TIMESTEP 1', f"MAX_TIMESTEP 1\nLTS_TIERS {job['lts_tiers']}")
    if not job.get('equalize', True):
        text = text.replace('COUPLING_POND_MODE CHAMBER',
                            'COUPLING_POND_MODE CHAMBER\nCOUPLING_EQUALIZE NO')
    return text


def simulate(job):
    folder = Path(job['folder'])
    folder.mkdir(parents=True, exist_ok=True)
    inp = folder/'model.inp'
    inp.write_text(deck(job))
    lib = ct.CDLL(job['library'])
    p, d, i = ct.c_void_p, ct.c_double, ct.c_int
    dp = ct.POINTER(d)
    signatures = {
        'swmm_engine_create': ([], p), 'swmm_engine_destroy': ([p], None),
        'swmm_engine_open': ([p,ct.c_char_p,ct.c_char_p,ct.c_char_p,p],i),
        'swmm_engine_initialize': ([p],i), 'swmm_engine_start': ([p,i],i),
        'swmm_engine_step': ([p,dp],i), 'swmm_engine_end': ([p],i),
        'swmm_engine_report': ([p],i), 'swmm_engine_close': ([p],i),
        'swmm_2d_triangle_count': ([p,ct.POINTER(i)],i),
        'swmm_2d_triangle_get_centroid': ([p,i,dp,dp,dp],i),
        'swmm_2d_get_coupling_volume_bulk': ([p,dp],i),
        'swmm_2d_get_depth': ([p,i,dp],i),
        'swmm_node_index': ([p,ct.c_char_p],i),
        'swmm_node_get_head': ([p,i,dp],i),
        'swmm_get_routing_continuity_error': ([p,dp],i),
        'swmm_get_routing_total': ([p,i,dp],i),
        'swmm_2d_get_continuity_error': ([p,dp],i),
        'swmm_2d_get_mass_balance': ([p]+[dp]*10,i),
        'swmm_2d_get_run_stats': ([p,ct.POINTER(Stats)],i),
    }
    for name,(args,result) in signatures.items():
        fn=getattr(lib,name); fn.argtypes=args; fn.restype=result
    diag = hasattr(lib,'rubinato_diag_reset')
    if diag:
        lib.rubinato_diag_reset.argtypes=[]; lib.rubinato_diag_reset.restype=None
        lib.rubinato_diag_read.argtypes=[dp]; lib.rubinato_diag_read.restype=None
    e=lib.swmm_engine_create()
    def call(name,*args):
        rc=getattr(lib,name)(e,*args)
        if rc: raise RuntimeError(f'{name}: {rc}')
    started=time.monotonic()
    try:
        call('swmm_engine_open',str(inp).encode(),str(folder/'model.rpt').encode(),b'',None)
        call('swmm_engine_initialize'); call('swmm_engine_start',0)
        n=i(); call('swmm_2d_triangle_count',ct.byref(n))
        volumes=(d*n.value)(); va=np.ctypeslib.as_array(volumes)
        mh=lib.swmm_node_index(e,b'MH')
        assert mh>=0
        nearest=(float('inf'),-1)
        for cell in range(n.value):
            x,y,z=d(),d(),d(); call('swmm_2d_triangle_get_centroid',cell,ct.byref(x),ct.byref(y),ct.byref(z))
            distance=math.hypot(x.value-3.54,y.value-2.0)
            if distance<nearest[0]: nearest=(distance,cell)
        elapsed=d(); prev_t=prev_v=0.0
        snapshots={0.0:0.0}; means=[0.0,0.0,0.0]
        min_step=float('inf'); max_step=0.0; step_count=0
        next_sample=0.0; reset=False
        with (folder/'series.csv').open('w',newline='') as f:
            w=csv.writer(f); w.writerow(['time_s','net_to_surface_m3','q_to_surface_lps','probe_depth_m','node_head_m'])
            while True:
                call('swmm_engine_step',ct.byref(elapsed))
                if elapsed.value==0: break
                t=elapsed.value*86400.0; dt=t-prev_t
                if dt<=0: raise RuntimeError('nonincreasing engine time')
                min_step=min(min_step,dt); max_step=max(max_step,dt); step_count+=1
                call('swmm_2d_get_coupling_volume_bulk',volumes); v=float(va.sum())
                hp,hs=d(),d()
                call('swmm_node_get_head',mh,ct.byref(hp)); call('swmm_2d_get_depth',nearest[1],ct.byref(hs))
                if not all(map(math.isfinite,[t,v,hp.value,hs.value])): raise RuntimeError('nonfinite state')
                for boundary in [180.0,240.0,300.0]:
                    if boundary not in snapshots and prev_t<boundary<=t+1e-7:
                        a=min(1.0,max(0.0,(boundary-prev_t)/dt))
                        snapshots[boundary]=prev_v+a*(v-prev_v)
                overlap=max(0.0,min(t,300.0)-max(prev_t,240.0))
                means[0]+=overlap*hp.value; means[1]+=overlap*hs.value; means[2]+=overlap
                if t+1e-7>=next_sample:
                    w.writerow([t,v,1000*(v-prev_v)/dt,hs.value,hp.value])
                    next_sample=math.floor(t+1e-7)+1.0
                if diag and not reset and t+1e-7>=240:
                    lib.rubinato_diag_reset(); reset=True
                prev_t,prev_v=t,v
        assert prev_t>=299.99 and len(snapshots)==4,(prev_t,snapshots)
        diagnostics=None
        if diag:
            values=(d*25)(); lib.rubinato_diag_read(values)
            diagnostics={k:float(v) if math.isfinite(v) else None for k,v in zip(DIAG_NAMES,values)}
        stats=Stats(); call('swmm_2d_get_run_stats',ct.byref(stats))
        call('swmm_engine_end')
        c1,c2=d(),d()
        call('swmm_get_routing_continuity_error',ct.byref(c1)); call('swmm_2d_get_continuity_error',ct.byref(c2))
        totals=[]
        for component in range(14):
            value=d(); rc=lib.swmm_get_routing_total(e,component,ct.byref(value))
            totals.append(value.value*0.3048**3 if rc==0 else None)
        surface=[d() for _ in range(10)]
        call('swmm_2d_get_mass_balance',*[ct.byref(v) for v in surface])
        call('swmm_engine_report'); call('swmm_engine_close')
        q=1000*(snapshots[300.0]-snapshots[240.0])/60.0
        qp=1000*(snapshots[240.0]-snapshots[180.0])/60.0
        result={**job,'status':'ok','wall_s':time.monotonic()-started,'duration_s':prev_t,
                'q_lps':q,'previous_q_lps':qp,'drift_pct':100*abs(q-qp)/max(abs(q),1e-12),
                'head_m':means[0]/means[2],'probe_depth_m':means[1]/means[2],
                'continuity_1d_pct':100*c1.value,'continuity_2d_pct':100*c2.value,
                'routing_steps':step_count,'actual_step_min_s':min_step,'actual_step_max_s':max_step,
                'surface_steps':stats.steps,'surface_average_step_s':prev_t/max(stats.steps,1),
                'backend':stats.backend.decode(),'routing_totals_m3':totals,
                'surface_balance_m3':dict(zip(['initial','final','rain','spill_in','drain_out',
                  'outfall_in','outfall_out','boundary_in','boundary_out','evap_out'],[v.value for v in surface])),
                'limiter_late_evaluations':diagnostics,'deck_sha256':hashlib.sha256(inp.read_bytes()).hexdigest()}
        text=(folder/'model.rpt').read_text(encoding='latin-1')
        for key,pattern in [('nonconverging_pct',r'% of Steps Not Converging\s*:\s*([\d.]+)'),
                            ('avg_iterations',r'Average Iterations per Step\s*:\s*([\d.]+)')]:
            match=re.search(pattern,text); result[key]=float(match[1]) if match else None
        return result
    finally:
        lib.swmm_engine_destroy(e)


def execute(job):
    folder=Path(job['folder']); folder.mkdir(parents=True,exist_ok=True)
    path=folder/'job.json'; path.write_text(json.dumps(job,indent=2)+'\n')
    env=os.environ.copy(); env['OMP_NUM_THREADS']='1'
    for k in ['OPENSWMM_2D_HEAD_RAMP','OPENSWMM_GPU_PLUGIN','OPENSWMM_GPU_BACKEND']:
        env.pop(k,None)
    with (folder/'run.log').open('w') as log:
        process=subprocess.run([sys.executable,str(Path(__file__).resolve()),'--job',str(path)],
                               stdout=log,stderr=subprocess.STDOUT,env=env)
    result=json.loads((folder/'result.json').read_text())
    print(job['name'],result['status'],f"{result.get('q_lps',float('nan')):.7g} L/s",flush=True)
    return result


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--job',type=Path); ap.add_argument('--library',type=Path)
    ap.add_argument('--output', type=Path)
    ap.add_argument('--input-directory', type=Path)
    ap.add_argument('--phase'); ap.add_argument('--steps',default='0.5,0.1,0.05,0.025,0.0125')
    ap.add_argument('--settings',default='iter80,tight4,tight5')
    ap.add_argument('--cases',default=','.join(CASES)); ap.add_argument('--workers',type=int,default=2)
    ap.add_argument('--no-equalize',action='store_true')
    ap.add_argument('--lts-tiers',type=int)
    ap.add_argument('--theta',type=float)
    ap.add_argument('--reference-step',type=float,default=0.0)
    args=ap.parse_args()
    if args.job:
        job=json.loads(args.job.read_text())
        try: result=simulate(job)
        except Exception as exc:
            import traceback
            traceback.print_exc(); result={**job,'status':'error','error':repr(exc)}
        (Path(job['folder'])/'result.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
        if result['status']!='ok': sys.exit(1)
        return
    if not (args.library and args.phase and args.output and args.input_directory):
        ap.error('--library, --phase, --output and --input-directory are required')
    library=args.library.resolve(); phase=args.output.resolve()/args.phase; phase.mkdir(parents=True,exist_ok=True)
    (phase/'library.json').write_text(json.dumps({'path':str(library),'sha256':hashlib.sha256(library.read_bytes()).hexdigest()},indent=2)+'\n')
    jobs=[]
    for case in args.cases.split(','):
        for step in map(float,args.steps.split(',')):
            for setting in args.settings.split(','):
                name=f'{case}_dt{step:g}_{setting}'
                jobs.append(dict(name=name,case=case,step=step,setting=setting,equalize=not args.no_equalize,
                                 library=str(library),input_directory=str(args.input_directory.resolve()),folder=str(phase/name),lts_tiers=args.lts_tiers,theta=args.theta,reference_step=args.reference_step))
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        results=list(pool.map(execute,jobs))
    (phase/'summary.json').write_text(json.dumps(results,indent=2,allow_nan=False)+'\n')
    if any(r['status']!='ok' for r in results): sys.exit(1)


if __name__=='__main__': main()
