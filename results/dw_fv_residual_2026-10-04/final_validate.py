from pathlib import Path
import hashlib,json,subprocess,re
R=Path(__file__).resolve().parent;E=R.parents[1];W=R.parent/'dw_fv_p0_2026-10-03';sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();m=json.loads((R/'manifest.json').read_text());bad=[p for p,h in m['files_sha256'].items() if sha(W/'source'/p)!=h];assert not bad,bad
h=sha(W/'build/src/engine/libopenswmm.engine.6.0.0.dylib');assert h==m['baseline_library_sha256']
production={p:sha(E/p) for p in ['src/engine/hydraulics/fv/ExplicitFvSolver.cpp','src/engine/hydraulics/fv/ExplicitFvSolver.hpp','src/engine/core/PerfTimers.hpp']};assert all(v==sha(R/'source_baseline'/p) for p,v in production.items())
assert '100% tests passed, 0 tests failed out of 6' in (R/'tests.log').read_text()
rows={folder:json.loads((R/folder/'results.json').read_text()) for folder in ('baseline_cost','checks','audit_checks','reuse_checks','pilot','confirmation')};assert {k:len(v) for k,v in rows.items()}=={'baseline_cost':4,'checks':60,'audit_checks':40,'reuse_checks':2,'pilot':54,'confirmation':54}
for folder in ('audit_checks','reuse_checks'):
 for p in (R/folder).glob('*.log'):
  with p.open() as f:
   for line in f:
    if line.startswith(('I ','R ','E ')):assert 'nan' not in line.lower() and 'inf' not in line.lower(),p
binaries=json.loads((R/'binaries.json').read_text())
for label,v in binaries.items():
 for p,x in v.items():assert sha(R/label/p)==x
changed=[p for p in m['files_sha256'] if sha(R/'source_candidate'/p)!=sha(R/'source_baseline'/p)];assert changed==['src/engine/hydraulics/fv/ExplicitFvSolver.cpp']
for name in ['DW_FV_RESIDUAL_CACHE_REPORT_2026-10-04.md','DW_FV_PERFORMANCE_REVIEW_AND_PLAN_2026-09-26.md']:
 p=E/'plans/1d'/name
 for link in re.findall(r'\]\(([^)]+)\)',p.read_text()):
  if '://' not in link and not link.startswith('#') and not link.endswith('final_validation.json'):assert (p.parent/link.split('#')[0]).exists(),link
result=dict(decision='retain isolated candidate; repeatable serial benefit, multithread elapsed-time gate open',candidate_changed_files=changed,selected_suites_passed=6,baseline_cost_runs=4,network_runs=60,internal_audit_runs=40,reuse_processes=2,reuse_engines_per_process=4,timing_runs=108,diagnostic_values_finite=True,candidate_internal_comparisons={k:sum(x[k] for x in rows['audit_checks'] if x['label']=='candidate') for k in ('trial_inputs','residuals','final_faces')},candidate_reuse_records=rows['reuse_checks'][1]['trace_records'],production_relevant_sha256=production,production_relevant_sources_unchanged=True,restored_source_files=len(m['files_sha256']),source_mismatches=bad,restored_library_sha256=h,restored_library_matches_accepted=True,library_sha256={label:sha(R/label/'libopenswmm.engine.6.dylib') for label in ('baseline','candidate','diagnostic_baseline','diagnostic_candidate')},root_head_at_finish=subprocess.check_output(['git','rev-parse','HEAD'],cwd=E,text=True).strip(),commit_created=False)
(R/'final_validation.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
