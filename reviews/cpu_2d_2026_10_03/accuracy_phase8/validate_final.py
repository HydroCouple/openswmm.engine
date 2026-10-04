from pathlib import Path
import json,subprocess,os,xml.etree.ElementTree as ET
p=Path(__file__).resolve().parent;q=p.parent/'accuracy_phase7';root=p.parents[2];cmds=json.loads((p/'engine_commands.json').read_text())[-2:]
for c in cmds:subprocess.run(c,cwd=root/'build/flow-trace',check=True)
rows=[]
for v,filter in [('baseline','*'),('selected','Cpu2DCorrectness.SecondOrderDistortedThackerKeepsShorelineVelocityBounded'),('connected_face','Cpu2DCorrectness.SecondOrderPlanarThackerRetainsRotationPhase')]:
 xml=p/f'final_{v}.xml';r=subprocess.run([str(p/'build_selected/test_engine_2d_cpu_correctness'),'--gtest_filter='+filter,'--gtest_output=xml:'+str(xml)],cwd=root/'tests/unit/engine/data',env={**os.environ,'OPENSWMM_2D_BACKEND':'cpu','DYLD_LIBRARY_PATH':str(p/f'build_{v}'),'DYLD_PRINT_LIBRARIES':'1'},capture_output=True,text=True,timeout=180);(p/f'final_{v}.log').write_text(r.stdout+r.stderr)
 assert str(p/f'build_{v}/libopenswmm.engine.6.dylib')in r.stderr or str(p/f'build_{v}/libopenswmm.engine.{v}.dylib')in r.stderr or(v=='baseline'and str(q/'build_selected/libopenswmm.engine.selected.dylib')in r.stderr)
 tree=ET.parse(xml).getroot();counts=dict(tree.attrib);counts['skipped']=len(tree.findall('.//skipped'));rows.append(dict(variant=v,code=r.returncode,counts=counts));print(v,r.returncode,counts,flush=True)
 if v=='baseline':assert r.returncode==0
 else:assert r.returncode!=0 and int(counts['failures'])==1
(p/'final_test_results.json').write_text(json.dumps(rows,indent=2))
# Remaining suites use phase-7's matched full engine and freshly added tests
# above; no production solver code is changed by phase 8.
regressions=[dict(suite='test_engine_2d_cpu_correctness',code=0,counts=rows[0]['counts'])]
for old in json.loads((q/'regression_results.json').read_text()):
 name=old['suite']
 if name=='test_engine_2d_cpu_correctness':continue
 xml=p/(name+'.xml');r=subprocess.run([str(q/'build_selected'/name),'--gtest_output=xml:'+str(xml)],cwd=root/'tests/unit/engine/data',env={**os.environ,'OPENSWMM_2D_BACKEND':'cpu','DYLD_LIBRARY_PATH':str(p/'build_baseline'),'DYLD_PRINT_LIBRARIES':'1'},capture_output=True,text=True,timeout=180);(p/(name+'.log')).write_text(r.stdout+r.stderr);assert str(q/'build_selected/libopenswmm.engine.selected.dylib')in r.stderr or str(p/'build_baseline/libopenswmm.engine.6.dylib')in r.stderr
 tree=ET.parse(xml).getroot();counts=dict(tree.attrib);counts['skipped']=len(tree.findall('.//skipped'));regressions.append(dict(suite=name,code=r.returncode,counts=counts));print(name,r.returncode,flush=True);assert r.returncode==0
 (p/'regression_results.json').write_text(json.dumps(regressions,indent=2))
