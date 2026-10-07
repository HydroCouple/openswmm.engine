from pathlib import Path
import json,subprocess,os,xml.etree.ElementTree as ET
p=Path(__file__).resolve().parent;q=p.parent/'cpu_phase5';root=p.parents[2];rows=[]
for item in json.loads((q/'regression_results.json').read_text()):
 name=item['suite'];xml=p/(name+'.xml');r=subprocess.run([str(p/'build_selected'/name),'--gtest_output=xml:'+str(xml)],cwd=root/'tests/unit/engine/data',env={**os.environ,'OPENSWMM_2D_BACKEND':'cpu','DYLD_LIBRARY_PATH':str(p/'build_selected'),'DYLD_PRINT_LIBRARIES':'1'},capture_output=True,text=True,timeout=180)
 (p/(name+'.log')).write_text(r.stdout+r.stderr);assert str(p/'build_selected/libopenswmm.engine.selected.dylib')in r.stderr or str(p/'build_selected/libopenswmm.engine.6.dylib')in r.stderr,name
 tree=ET.parse(xml).getroot();counts=dict(tree.attrib);counts['skipped']=len(tree.findall('.//skipped'));rows.append(dict(suite=name,code=r.returncode,counts=counts));print(name,r.returncode,counts['tests'],flush=True)
 (p/'regression_results.json').write_text(json.dumps(rows,indent=2))
assert all(r['code']==0 for r in rows)
# The two new physics tests must reject the old implementation independently.
f='Cpu2DCorrectness.SecondOrderSlopeReconstructionCarriesPhysicalMassFlux:Cpu2DCorrectness.SecondOrderPlanarThackerRetainsRotationPhase';r=subprocess.run([str(p/'build_baseline/test_engine_2d_cpu_correctness'),'--gtest_filter='+f,'--gtest_output=xml:'+str(p/'new_tests_baseline.xml')],env={**os.environ,'OPENSWMM_2D_BACKEND':'cpu','DYLD_LIBRARY_PATH':str(p/'build_baseline'),'DYLD_PRINT_LIBRARIES':'1'},capture_output=True,text=True,timeout=180);(p/'new_tests_baseline.log').write_text(r.stdout+r.stderr)
assert (str(p/'build_baseline/libopenswmm.engine.baseline.dylib')in r.stderr or str(p/'build_baseline/libopenswmm.engine.6.dylib')in r.stderr),'baseline library not confirmed';tree=ET.parse(p/'new_tests_baseline.xml').getroot();assert r.returncode!=0 and int(tree.attrib['failures'])==2,tree.attrib
print('PASS selected suites; both new regressions fail baseline as expected',flush=True)
