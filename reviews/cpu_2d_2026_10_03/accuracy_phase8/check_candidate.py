from pathlib import Path
import subprocess,json,os,xml.etree.ElementTree as ET
p=Path(__file__).resolve().parent;root=p.parents[2];rows=[]
for v,f in [('selected','*'),('baseline','Cpu2DCorrectness.SecondOrderShorelineKeepsConnectedWetFaceAccuracy'),('connected_face','Cpu2DCorrectness.SecondOrderPlanarThackerRetainsRotationPhase')]:
 xml=p/f'correctness_{v}.xml';r=subprocess.run([str(p/'build_selected/test_engine_2d_cpu_correctness'),'--gtest_filter='+f,'--gtest_output=xml:'+str(xml)],cwd=root/'tests/unit/engine/data',env={**os.environ,'OPENSWMM_2D_BACKEND':'cpu','DYLD_LIBRARY_PATH':str(p/f'build_{v}'),'DYLD_PRINT_LIBRARIES':'1'},capture_output=True,text=True,timeout=180)
 (p/f'correctness_{v}.log').write_text(r.stdout+r.stderr)
 assert str(p/f'build_{v}/libopenswmm.engine.6.dylib')in r.stderr or str(p/f'build_{v}/libopenswmm.engine.{v}.dylib')in r.stderr or (v=='baseline'and str(p.parent/'accuracy_phase7/build_selected/libopenswmm.engine.selected.dylib')in r.stderr)
 tree=ET.parse(xml).getroot();rows.append(dict(variant=v,returncode=r.returncode,counts=tree.attrib));print(v,r.returncode,tree.attrib,flush=True);(p/'correctness_results.json').write_text(json.dumps(rows,indent=2))
