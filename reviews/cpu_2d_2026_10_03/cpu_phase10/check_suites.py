from pathlib import Path
import json,subprocess,os,sys,xml.etree.ElementTree as ET
p=Path(__file__).resolve().parent;q=p.parent/'accuracy_phase7';root=p.parents[2];v=sys.argv[1]if len(sys.argv)>1 else 'connected_first_velocity';rows=[]
for old in json.loads((q/'regression_results.json').read_text()):
 name=old['suite'];exe=p/f'build_{v}'/name if name=='test_engine_2d_cpu_correctness'else q/'build_selected'/name;xml=p/f'{v}_{name}.xml'
 r=subprocess.run([str(exe),'--gtest_output=xml:'+str(xml)],cwd=root/'tests/unit/engine/data',env={**os.environ,'OPENSWMM_2D_BACKEND':'cpu','DYLD_LIBRARY_PATH':str(p/f'build_{v}'),'DYLD_PRINT_LIBRARIES':'1'},capture_output=True,text=True,timeout=180);(p/f'{v}_{name}.log').write_text(r.stdout+r.stderr)
 assert str(p/f'build_{v}/libopenswmm.engine.6.dylib')in r.stderr or str(p/f'build_{v}/libopenswmm.engine.{v}.dylib')in r.stderr
 tree=ET.parse(xml).getroot();counts=dict(tree.attrib);counts['skipped']=len(tree.findall('.//skipped'));rows.append(dict(suite=name,code=r.returncode,counts=counts));print(name,r.returncode,counts,flush=True);(p/f'regressions_{v}.json').write_text(json.dumps(rows,indent=2))
assert all(x['code']==0 for x in rows)
