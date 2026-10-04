#!/usr/bin/env python3
"""Persistent synthetic-network screening; independent processes and paired order.
This is a timing screen, not an accuracy/production performance certification.
"""
import ctypes as C
import hashlib,json,math,os,re,struct,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parent

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def deck(solver,cells,threads,n=1024,duration=30,pressure=False):
    depth=5.0 if pressure else 1.0
    theta=2*math.acos(1-depth/2) if not pressure else 2*math.pi
    area=2*(theta-math.sin(theta));radius=area/(2*theta)
    flow=1.486/.013*area*radius**(2/3)*math.sqrt(.001)
    s=f'''[TITLE]
Wet chain screen; {n} authored conduits, 40 feet each, 4-foot circles.
[OPTIONS]
FLOW_UNITS CFS
FLOW_ROUTING {solver}
SURCHARGE_METHOD SLOT
START_DATE 01/01/2026
END_DATE 01/01/2026
START_TIME 00:00:00
END_TIME 00:{duration//60:02d}:{duration%60:02d}
REPORT_STEP 1
ROUTING_STEP 0.25
VARIABLE_STEP 0.75
LENGTHENING_STEP 0
SKIP_STEADY_STATE NO
THREADS {threads}
FV_MIN_CELLS {cells}
FV_LTS YES
FV_ORDER 1
[JUNCTIONS]
'''
    s+=''.join(f'J{i} {100-i*.04:.8f} 12 {depth} 0 0\n' for i in range(n))
    s+=f'[OUTFALLS]\nOUT {100-n*.04:.8f} FIXED {100-n*.04+depth:.8f} NO\n[CONDUITS]\n'
    s+=''.join(f'C{i} J{i} '+(f'J{i+1}' if i+1<n else 'OUT')+f' 40 .013 0 0 {flow:.17g}\n' for i in range(n))
    s+='[XSECTIONS]\n'+''.join(f'C{i} CIRCULAR 4 0 0 0 1\n' for i in range(n))
    s+=f'[DWF]\nJ0 FLOW {flow:.17g}\n[REPORT]\nNODES ALL\nLINKS ALL\n'
    return s

def worker(label,solver,cells,threads,duration,pressure,out):
    out=Path(out);out.mkdir(parents=True,exist_ok=False)
    inp=out/'input.inp';inp.write_text(deck(solver,cells,threads,duration=duration,pressure=pressure))
    t0=time.perf_counter()
    C.CDLL('/opt/homebrew/opt/libomp/lib/libomp.dylib',mode=C.RTLD_GLOBAL)
    lib=C.CDLL(str(ROOT/label/'libopenswmm.engine.6.dylib'))
    def bind(n,restype,args):
        f=getattr(lib,n);f.restype=restype;f.argtypes=args;return f
    p,i,d,s=C.c_void_p,C.c_int,C.c_double,C.c_char_p
    create=bind('swmm_engine_create',p,[]);destroy=bind('swmm_engine_destroy',None,[p])
    err=bind('swmm_get_last_error_msg',s,[p])
    op=bind('swmm_engine_open',i,[p,s,s,s,s]);init=bind('swmm_engine_initialize',i,[p]);start=bind('swmm_engine_start',i,[p,i])
    step=bind('swmm_engine_step',i,[p,C.POINTER(d)]);end=bind('swmm_engine_end',i,[p]);report=bind('swmm_engine_report',i,[p]);close=bind('swmm_engine_close',i,[p])
    eff=bind('swmm_get_effective_threads',i,[p,i,C.POINTER(i),C.POINTER(i),C.POINTER(i)])
    class Info(C.Structure):_fields_=[(k,i) for k in ('logical_cpus','omp_max_threads','omp_available','perf_cores','kokkos_omp_threads')]
    info=Info();bind('swmm_get_thread_info',i,[C.POINTER(Info)])(C.byref(info))
    e=create()
    def check(code):
        if code:raise RuntimeError(f'{code}: {err(e)!r}')
    try:
        check(op(e,bytes(inp),bytes(out/'report.rpt'),bytes(out/'output.out'),None));check(init(e));check(start(e,1))
        g,w,t=i(),i(),i();check(eff(e,threads,C.byref(g),C.byref(w),C.byref(t)))
        elapsed=d();routing=0.;count=0;last=0.
        with (out/'step_times.f64').open('wb') as f:
            for _ in range(100000):
                before=time.perf_counter();check(step(e,C.byref(elapsed)));routing+=time.perf_counter()-before
                f.write(struct.pack('<d',elapsed.value));count+=1
                if elapsed.value==0:break
                last=elapsed.value*86400
            else:raise RuntimeError('step limit')
        assert last>=duration-.51,last
        check(end(e));check(report(e))
        rpt=(out/'report.rpt').read_text()
        row=dict(label=label,solver=solver,min_cells=cells,requested_threads=threads,pressure=pressure,duration=duration,
                 engine_step_seconds=routing,total_api_seconds=time.perf_counter()-t0,steps=count,last_time=last,
                 effective_global=g.value,effective_dw=w.value,hardware={k:getattr(info,k) for k,_ in info._fields_},
                 output_sha256=sha(out/'output.out'),step_times_sha256=sha(out/'step_times.f64'),input_sha256=sha(inp),
                 library_sha256=sha(ROOT/label/'libopenswmm.engine.6.dylib'),load_average=os.getloadavg())
        for key,pattern in [('continuity_percent',r'Flow Routing Continuity.*?Continuity Error \(%\)\s*\.*\s*(-?[0-9.]+)'),
          ('substeps',r'Explicit Substeps\s*\.*\s*(\d+)'),('fluxes',r'Face Flux Evaluations\s*\.*\s*(\d+)'),('macros',r'LTS Macro Cycles Fired\s*\.*\s*(\d+)')]:
            m=re.search(pattern,rpt,re.S)
            if m:row[key]=float(m[1])
        (out/'result.json').write_text(json.dumps(row,indent=2))
        check(close(e)) # emits the FV profile at the documented close boundary
    finally:destroy(e)

def run(name,configs,repeats=3,duration=30):
    out=ROOT/name;out.mkdir(exist_ok=False);rows=[]
    for solver,cells,threads,pressure in configs:
        for repeat in range(repeats):
            for label in (('baseline','candidate') if repeat%2==0 else ('candidate','baseline')):
                case=f'{solver}_m{cells}_t{threads}_p{int(pressure)}_r{repeat}_{label}';dest=out/case
                env={k:v for k,v in os.environ.items() if not k.startswith(('OMP_','KMP_','OPENSWMM_','SWMM_','DYLD_'))}
                env.update(OMP_NUM_THREADS=str(threads),OMP_THREAD_LIMIT=str(threads),OMP_DYNAMIC='FALSE',OMP_WAIT_POLICY='PASSIVE',KMP_BLOCKTIME='0')
                args=[sys.executable,str(Path(__file__).resolve()),'worker',label,solver,str(cells),str(threads),str(duration),str(int(pressure)),str(dest)]
                with (out/f'{case}.log').open('w') as log:
                    try:
                        res=subprocess.run(args,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=120)
                        row=json.loads((dest/'result.json').read_text()) if res.returncode==0 else {'returncode':res.returncode}
                    except subprocess.TimeoutExpired:row={'error':'timeout'}
                row.update(case=case,repeat=repeat);rows.append(row)
                (out/'results.json').write_text(json.dumps(rows,indent=2));print(case,row.get('engine_step_seconds',row),flush=True)
if __name__=='__main__':
    if sys.argv[1]=='worker':worker(sys.argv[2],sys.argv[3],int(sys.argv[4]),int(sys.argv[5]),int(sys.argv[6]),bool(int(sys.argv[7])),sys.argv[8])
    elif sys.argv[1]=='pilot':run('network_pilot',[('DYNWAVE',4,1,False),('FV',4,1,False),('DYNWAVE',4,1,True),('FV',4,1,True)],repeats=1,duration=5)
    else:run('network_screen',[(s,m,t,False) for s,m,t in [('DYNWAVE',4,t) for t in (1,2,4,8)]+[('FV',4,t) for t in (1,2,4,8)]+[('FV',m,1) for m in (8,16)]],repeats=5,duration=300)
