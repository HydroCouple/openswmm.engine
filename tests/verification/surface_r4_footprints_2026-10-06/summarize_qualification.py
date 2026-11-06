from pathlib import Path
import json,hashlib,subprocess,shutil
repo=Path(__file__).resolve().parents[3];e=Path(__file__).resolve().parent
a=json.loads((e/'source-audit.json').read_text());source=Path(a['build_source_path'])
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
for p,d in a['files'].items():
 if p.startswith('plans/'):continue
 assert sha(repo/p)==d==sha(source/p),p
paths=['src/engine/hydrology/LidNode.cpp','src/engine/hydrology/LidNode.hpp','src/engine/hydrology/RichardsColumn.cpp','src/engine/hydrology/RichardsColumn.hpp','src/engine/hydraulics/Node.cpp','src/engine/2d/subsurface/SigmaColumn.cpp','src/engine/2d/subsurface/SubsurfaceSolver.cpp','src/engine/2d/SurfaceRouter2D.cpp']
for p in paths:assert (repo/p).read_bytes()==subprocess.check_output(['git','show',a['head']+':'+p],cwd=repo)==(source/p).read_bytes(),p
a['unchanged_controls']={p:sha(repo/p) for p in paths}
a['library_sha256']=sha(repo/'build/surface-r4-isolated/src/engine/libopenswmm.engine.6.0.0.dylib')
a['final_documentation_sha256']={p:sha(repo/p) for p in ['plans/SURFACE_PROCESS_R4_PARTIAL_FOOTPRINTS_2026-10-06.md','tests/verification/surface_r4_footprints_2026-10-06/README.md']}
a['qualification_runner_sha256']={p:sha(e/p) for p in ['prepare_source.py','run_tests.py','run_controls.py','ordinary_controls.cpp','summarize_qualification.py']}
(e/'source-audit.json').write_text(json.dumps(a,indent=2)+'\n')
suites={}
for p in sorted((e/'qualified-cases').glob('*.json')):
 d=json.loads(p.read_text());assert d['failures']==0;suites[p.stem]={'cases':d['tests'],'failures':d['failures']}
c={'suites':suites,'total':sum(d['cases'] for d in suites.values()),'unique_run':'qualified-cases'};assert c['total']==311 and len(suites)==15
(e/'case-counts.json').write_text(json.dumps(c,indent=2)+'\n')
assert 'ALL GATES PASS' in (e/'qualified-gates.log').read_text()
assert json.loads((e/'ordinary-controls-summary.json').read_text())['byte_identical']
shutil.copy2(source/'tests/verification/surface_r4_water_2026-10-05/models/swale-refinement.jsonl',e/'swale-refinement.jsonl')
print('Verified 311 cases, groundwater gates, byte-identical ordinary controls and eight preserved production files.')
