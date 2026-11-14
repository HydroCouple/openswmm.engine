from pathlib import Path
import subprocess,json,time,sys
repo=Path(__file__).resolve().parents[3];e=Path(__file__).resolve().parent
suffix=sys.argv[1] if len(sys.argv)>1 else 'first'
names=sys.argv[2:] or ['test_engine_common_source_stage','test_engine_source_water_driver','test_engine_surface_et_budget']
case_dir=e/(suffix+'-cases');case_dir.mkdir(parents=True,exist_ok=True)
results={}
with (e/(suffix+'-tests.log')).open('w') as log:
 for name in names:
  t=time.monotonic()
  p=subprocess.run([str(repo/'build/surface-r4-isolated/bin/Release'/name),'--gtest_color=no','--gtest_output=json:'+str(case_dir/(name+'.json'))],stdout=log,stderr=subprocess.STDOUT,cwd=repo/'build/surface-r4-isolated')
  results[name]={'exit_code':p.returncode,'seconds':time.monotonic()-t}
  print(name,p.returncode,flush=True)
(e/(suffix+'-results.json')).write_text(json.dumps(results,indent=2)+'\n')
sys.exit(any(v['exit_code'] for v in results.values()))
