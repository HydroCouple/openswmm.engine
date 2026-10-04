from pathlib import Path
import json,hashlib,difflib,subprocess,math,xml.etree.ElementTree as ET
p=Path(__file__).resolve().parent;root=p.parents[2];f=p/'final_verification.json';meta=json.loads(f.read_text());patch=[]
access='public:\n    const std::vector<double>& reviewQx()const{return qcx_;}\n    const std::vector<double>& reviewQy()const{return qcy_;}\nprivate:'
for rel in meta['production_hashes']:
 old=(p/'baseline_test.cpp').read_text()if rel.startswith('tests/')else(p/'src_baseline'/rel).read_text().replace(access,'private:')
 assert hashlib.sha256(old.encode()).hexdigest()==meta['previous_hashes'][rel]
 new=(root/rel).read_text();selected=(p/'test_sources/test_2d_cpu_correctness.cpp').read_text()if rel.startswith('tests/')else(p/'src_selected'/rel).read_text().replace(access,'private:');assert new==selected,rel
 meta['production_hashes'][rel]=hashlib.sha256(new.encode()).hexdigest();patch.extend(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+rel,tofile='b/'+rel))
(p/'selected.patch').write_text(''.join(patch))
counts={'diagnosis':20,'screen':24,'accuracy_results':56,'compatibility_results':72,'thread_results':8,'performance_results':50}
for name,n in counts.items():
 rows=json.loads((p/(name+'.json')).read_text());assert len(rows)==n,(name,len(rows));assert all(math.isfinite(v)for r in rows for v in r.values()if isinstance(v,float)),name
rows=json.loads((p/'regression_results.json').read_text());assert all(r['code']==0 and int(r['counts']['failures'])==0 and int(r['counts']['errors'])==0 for r in rows)
assert sum(int(r['counts']['tests'])-int(r['counts']['skipped'])for r in rows)==166
assert sum(int(r['counts']['skipped'])for r in rows)==1
assert int(ET.parse(p/'new_tests_baseline.xml').getroot().attrib['failures'])==2
prototype=json.loads((p/'accuracy_prototype_results.json').read_text());final=json.loads((p/'accuracy_results.json').read_text());assert all({k:v for k,v in a.items()if k!='seconds'}=={k:v for k,v in b.items()if k!='seconds'}for a,b in zip(prototype,final))
subprocess.run(['git','diff','--check','--',*meta['production_hashes']],cwd=root,check=True)
meta['engine_binary_hashes']={v:hashlib.sha256((p/f'build_{v}/libopenswmm.engine.{v}.dylib').read_bytes()).hexdigest()for v in ['baseline','selected']}
meta['evidence_hashes']={name:hashlib.sha256((p/name).read_bytes()).hexdigest()for name in ['REPORT.md','selected.patch','accuracy_results.json','compatibility_results.json','thread_results.json','regression_results.json','performance_results.json','new_tests_baseline.xml','comparison.png']};meta['vfr_emerged_lake_checked']=True;meta['object_layout_changed']=False
f.write_text(json.dumps(meta,indent=2)+'\n');print('Verified applied code, both new tests, FLAT/VFR stationary balance and all final evidence counts.')
