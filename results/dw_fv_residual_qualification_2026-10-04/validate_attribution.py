from pathlib import Path
import os,sys,subprocess,json,shutil
Q=Path(__file__).resolve().parent;R=Q.parent/'dw_fv_residual_2026-10-04';rows=json.loads((Q/'attribution/results.json').read_text());out=Q/'attribution_controls';out.mkdir(exist_ok=False);(Q/'clean_baseline').mkdir(exist_ok=True);shutil.copyfile(R/'baseline/libopenswmm.engine.6.dylib',Q/'clean_baseline/libopenswmm.engine.6.dylib');controls=[]
for case,c in [('free',4),('free',16),('rest',4)]:
 name=f'{case}_c{c}';env={k:v for k,v in os.environ.items() if not k.startswith(('OMP_','KMP_','OPENSWMM_','SWMM_','DYLD_','FV_'))};env.update(OMP_NUM_THREADS='1',OMP_THREAD_LIMIT='1',OMP_DYNAMIC='FALSE',OMP_WAIT_POLICY='PASSIVE',KMP_BLOCKTIME='0')
 cp=subprocess.run([sys.executable,str(Q/'attribution.py'),'worker','clean_baseline',case,str(c),'1',str(out/name)],env=env,capture_output=True,text=True,timeout=180);(out/(name+'.log')).write_text(cp.stdout+cp.stderr);assert cp.returncode==0
 ref=json.loads((out/name/'result.json').read_text());keys=('output_sha256','step_times_sha256','substeps','fluxes','macros')
 matched=[r for r in rows if (r['case'],r['cells'])==(case,c)];assert len(matched)==12
 for row in matched:assert all(row['result'][k]==ref[k] for k in keys),row['name']
 controls.append(dict(case=case,cells=c,matched_runs=len(matched),signature={k:ref[k] for k in keys}));(out/'results.json').write_text(json.dumps(controls,indent=2));print(name,'matches 12 instrumented runs',flush=True)
