from pathlib import Path
import json,hashlib,re,subprocess
Q=Path(__file__).resolve().parent;R=Q.parent/'dw_fv_residual_2026-10-04';W=Q.parent/'dw_fv_p0_2026-10-03';E=Q.parents[1]
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
m=json.loads((R/'manifest.json').read_text());bad=[p for p,h in m['files_sha256'].items() if sha(W/'source'/p)!=h];assert not bad,bad
library=sha(W/'build/src/engine/libopenswmm.engine.6.0.0.dylib');assert library==m['baseline_library_sha256']
production={p:sha(E/p) for p in ('src/engine/hydraulics/fv/ExplicitFvSolver.cpp','src/engine/hydraulics/fv/ExplicitFvSolver.hpp','src/engine/core/PerfTimers.hpp')};assert all(v==sha(R/'source_baseline'/p) for p,v in production.items())
q=json.loads((Q/'checks/results.json').read_text());a=json.loads((Q/'attribution/results.json').read_text());c=json.loads((Q/'attribution_controls/results.json').read_text());assert (len(q),len(a),len(c))==(32,36,3)
for x in q:
 ref=next(v for v in q if (v['case'],v['rejection'])==(x['case'],x['rejection']))
 assert x['signature']==ref['signature']
 assert x['observed_teams']==[x['threads']]
 assert x['records'].get('H',0)==0
 if x['rejection']:assert x['profile']['n.rollback']==1 and x['restore_cell_records']==96
for x in a:
 ref=next(v for v in c if (v['case'],v['cells'])==(x['case'],x['cells']))
 assert all(x['result'][k]==v for k,v in ref['signature'].items())
 assert x['node_solves']==x['regions']*256
for label in ('baseline','candidate'):
 assert 'qualificationReject' not in (Q/f'attribution_{label}.cpp').read_text()
 assert 'qualificationReject' in (Q/f'{label}.cpp').read_text()
for label,files in json.loads((R/'binaries.json').read_text()).items():
 for p,h in files.items():assert sha(R/label/p)==h
for name in ('DW_FV_RESIDUAL_QUALIFICATION_REPORT_2026-10-04.md','DW_FV_RESIDUAL_CACHE_REPORT_2026-10-04.md','DW_FV_PERFORMANCE_REVIEW_AND_PLAN_2026-09-26.md'):
 p=E/'plans/1d'/name
 for link in re.findall(r'\]\(([^)]+)\)',p.read_text()):
  if '://' not in link and not link.startswith('#') and not link.endswith('final_validation.json'):assert (p.parent/link.split('#')[0]).exists(),link
result=dict(decision='retain isolated; synthetic retry and culvert fallback qualified, elapsed-time scaling gate still open',qualification_runs=len(q),attribution_runs=len(a),clean_numerical_control_runs=len(c),candidate_comparison_records={k:sum(x['records'].get(k,0) for x in q if x['label']=='candidate') for k in ('I','R','E','CELL','REJECT','H','SPECIAL')},source_files_restored=len(m['files_sha256']),source_mismatches=bad,restored_library_sha256=library,accepted_library_matches=True,prior_frozen_libraries_unchanged=True,production_relevant_sha256=production,production_relevant_unchanged=True,library_sha256={label:sha(Q/label/'libopenswmm.engine.6.dylib') for label in ('baseline','candidate','attribution_baseline','attribution_candidate')},artifact_sha256={str(p.relative_to(Q)):sha(p) for p in Q.glob('*') if p.suffix in ('.py','.cpp','.patch')},root_head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=E,text=True).strip(),commit_created=False)
(Q/'final_validation.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
