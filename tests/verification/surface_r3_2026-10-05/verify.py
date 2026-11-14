from pathlib import Path
import subprocess,json,os,sys
root=Path.cwd();out=root/'tests/verification/surface_r3_2026-10-05';isolated=len(sys.argv)>1;build=out/'isolated/build' if isolated else root/'build/surface-program';logs=out/'isolated' if isolated else out;rows=[]
for target in ['test_engine_surface_et_budget','test_engine_surface_aquifer_receiving','test_engine_2d_infil','test_engine_2d_aquifer','test_engine_hotstart','test_engine_gw_transport_kernel','gw2d_gates']:
 exe=build/'bin/Release'/target if target!='gw2d_gates' else build/'tests/verification/gw2d_gates/gw2d_gates'
 p=subprocess.run([str(exe)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT);s=p.stdout.decode(errors='replace');(logs/(target+'.log')).write_text(s.rstrip()+'\n');r={'target':target,'returncode':p.returncode,'summary':[l for l in s.splitlines() if 'PASSED' in l or 'FAILED' in l or 'ALL GATES' in l]};rows.append(r);print(r,flush=True)
(logs/'test_summary.json').write_text(json.dumps(rows,indent=2)+'\n');assert all(x['returncode']==0 for x in rows)
