from pathlib import Path
import json,subprocess,os,xml.etree.ElementTree as ET
p=Path(__file__).resolve().parent;root=p.parents[2];rel='tests/unit/engine/test_2d_cpu_correctness.cpp';f=root/rel;s=f.read_text();a=s.index('TEST(Cpu2DCorrectness,LakeAtRestAgainstAnEmergedBump)');b=s.index('\nTEST(',a+1);part=s[a:b];part=part.replace('for(bool quad:{false,true})for(int order:{1,2}){','for(bool quad:{false,true})for(int order:{1,2})for(auto closure:{CellClosure2D::FLAT,CellClosure2D::VFR}){').replace('o.momentum=Momentum2D::FULL_SWE;o.reconstruction_order=order;', 'o.momentum=Momentum2D::FULL_SWE;o.reconstruction_order=order;o.cell_closure=closure;');assert part!=s[a:b];s=s[:a]+part+s[b:];f.write_text(s);(p/'test_sources/test_2d_cpu_correctness.cpp').write_text(s)
cmds=json.loads((p/'test_commands.json').read_text());selected=[c for c in cmds if any(str(p/'build_selected/test_2d_cpu_correctness.o')==x for x in c)]
assert len(selected)==2,len(selected)
for cmd in selected:subprocess.run(cmd,cwd=root/'build/flow-trace',check=True)
name='test_engine_2d_cpu_correctness';xml=p/(name+'.xml');r=subprocess.run([str(p/'build_selected'/name),'--gtest_output=xml:'+str(xml)],cwd=root/'tests/unit/engine/data',env={**os.environ,'OPENSWMM_2D_BACKEND':'cpu','DYLD_LIBRARY_PATH':str(p/'build_selected'),'DYLD_PRINT_LIBRARIES':'1'},capture_output=True,text=True);(p/(name+'.log')).write_text(r.stdout+r.stderr);r.check_returncode();assert str(p/'build_selected/libopenswmm.engine.selected.dylib')in r.stderr or str(p/'build_selected/libopenswmm.engine.6.dylib')in r.stderr
rows=json.loads((p/'regression_results.json').read_text());tree=ET.parse(xml).getroot();counts=dict(tree.attrib);counts['skipped']=len(tree.findall('.//skipped'))
for row in rows:
 if row['suite']==name:row.update(code=0,counts=counts)
(p/'regression_results.json').write_text(json.dumps(rows,indent=2));print('PASS all 18 correctness tests, including FLAT and VFR emerged-lake balance',flush=True)
