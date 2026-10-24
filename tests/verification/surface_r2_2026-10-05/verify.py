from pathlib import Path
import subprocess,json,os,sys
root=Path.cwd();out=root/'tests/verification/surface_r2_2026-10-05';isolated=len(sys.argv)>1
build=out/'isolated/build' if isolated else root/'build/surface-program';logs=out/'isolated' if isolated else out
summary=[];env=dict(os.environ,OPENSWMM_R2_OUT=str(logs/'models'))
for target in ['test_engine_surface_aquifer_receiving','test_engine_2d_infil','test_engine_2d_aquifer','test_engine_hotstart','test_engine_gw_transport_kernel','gw2d_gates']:
 exe=build/'bin/Release'/target if target!='gw2d_gates' else build/'tests/verification/gw2d_gates/gw2d_gates'
 run=subprocess.run([str(exe)],cwd=root,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT);body=run.stdout.decode(errors='replace');(logs/(target+'.log')).write_text(body)
 item={'target':target,'returncode':run.returncode,'summary':[line for line in body.splitlines() if 'PASSED' in line or 'FAILED' in line or 'ALL GATES' in line]};summary.append(item);print(item,flush=True)
(logs/'test_summary.json').write_text(json.dumps(summary,indent=2)+'\n')
assert all(x['returncode']==0 for x in summary)
