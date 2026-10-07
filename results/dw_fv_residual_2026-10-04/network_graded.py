#!/usr/bin/env python3
"""Authored-conduit LTS fixture, retaining four cells per conduit and fixed physics."""
import math,sys,json,os,subprocess,re
from pathlib import Path
import network_screen as net
ROOT=Path(__file__).resolve().parent

def deck(solver,cells,threads,**unused):
    n=1024;duration=300;lengths=[5. if 510<=i<514 else 400. for i in range(n)]
    z=[1000.]
    for length in lengths:z.append(z[-1]-.001*length)
    theta=2*math.acos(.5);area=2*(theta-math.sin(theta));radius=area/(2*theta)
    q=1.486/.013*area*radius**(2/3)*math.sqrt(.001)
    s=f'''[TITLE]
Graded wet chain with a localized four-conduit short reach.
[OPTIONS]
FLOW_UNITS CFS
FLOW_ROUTING {solver}
START_DATE 01/01/2026
END_DATE 01/01/2026
START_TIME 00:00:00
END_TIME 00:05:00
REPORT_STEP 10
ROUTING_STEP 5
VARIABLE_STEP 0.75
LENGTHENING_STEP 0
SKIP_STEADY_STATE NO
THREADS {threads}
FV_MIN_CELLS {cells}
FV_LTS YES
FV_ORDER 1
[JUNCTIONS]
'''
    s+=''.join(f'J{i} {z[i]:.12f} 12 1 0 0\n' for i in range(n))
    s+=f'[OUTFALLS]\nOUT {z[-1]:.12f} FIXED {z[-1]+1:.12f} NO\n[CONDUITS]\n'
    s+=''.join(f'C{i} J{i} '+(f'J{i+1}' if i+1<n else 'OUT')+f' {lengths[i]} .013 0 0 {q:.17g}\n' for i in range(n))
    s+='[XSECTIONS]\n'+''.join(f'C{i} CIRCULAR 4 0 0 0 1\n' for i in range(n))
    return s+f'[DWF]\nJ0 FLOW {q:.17g}\n[REPORT]\nNODES ALL\nLINKS ALL\n'
if len(sys.argv)==5:
    net.deck=deck;net.worker(sys.argv[1],'FV',int(sys.argv[2]),int(sys.argv[3]),300,False,sys.argv[4])
else:
    out=ROOT/sys.argv[1];out.mkdir(exist_ok=False);rows=[]
    label_candidate=sys.argv[3] if len(sys.argv)>3 else 'candidate'
    for cells,threads in [(4,1),(4,4),(8,1),(16,1)]:
        for repeat in range(int(sys.argv[2])):
            for label in (('baseline',label_candidate) if repeat%2==0 else (label_candidate,'baseline')):
                name=f'm{cells}_t{threads}_r{repeat}_{label}';dest=out/name
                env={k:v for k,v in os.environ.items() if not k.startswith(('OMP_','KMP_','OPENSWMM_','SWMM_','DYLD_'))}
                env.update(OMP_NUM_THREADS=str(threads),OMP_THREAD_LIMIT=str(threads),OMP_DYNAMIC='FALSE',OMP_WAIT_POLICY='PASSIVE',KMP_BLOCKTIME='0')
                res=subprocess.run([sys.executable,__file__,label,str(cells),str(threads),str(dest)],env=env,text=True,capture_output=True,timeout=120)
                (out/f'{name}.log').write_text(res.stdout+res.stderr);assert res.returncode==0,(name,res.returncode,res.stderr)
                row=json.loads((dest/'result.json').read_text());row.update(name=name,repeat=repeat)
                assert abs(row['continuity_percent'])<.1 and row['macros']>0,row
                reference=next((x for x in rows if x['min_cells']==cells),None)
                if reference:assert row['output_sha256']==reference['output_sha256'] and row['step_times_sha256']==reference['step_times_sha256'],name
                rows.append(row);(out/'results.json').write_text(json.dumps(rows,indent=2));print(name,round(row['engine_step_seconds'],4),row['macros'],flush=True)
