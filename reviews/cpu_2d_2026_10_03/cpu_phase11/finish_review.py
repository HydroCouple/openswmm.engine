from pathlib import Path
import json,hashlib,statistics,subprocess
p=Path(__file__).resolve().parent;root=p.parents[2];manifest=json.loads((p/'baseline_manifest.json').read_text())
for f,h in manifest.items():assert hashlib.sha256((root/f).read_bytes()).hexdigest()==h,('Source drift',f)
screen=json.loads((p/'screen.json').read_text());confirm=json.loads((p/'confirmation.json').read_text());coupled=json.loads((p/'coupled_probe.json').read_text());ss=json.loads((p/'screen_summary.json').read_text());cs=json.loads((p/'confirmation_summary.json').read_text());cp=json.loads((p/'coupled_probe_summary.json').read_text())
assert len(screen)==96 and len(confirm)==36 and len(coupled)==8
# Revalidate the comparison records without relying on the run scripts' assertions.
ignore={'seconds','variant','repeat','cpu','load'}
for rows in [screen,confirm]:
 groups={}
 for r in rows:
  key=(r['case'],r['nx'],r['quad'],r['threads'],r['repeat']);groups.setdefault(key,[]).append({k:v for k,v in r.items()if k not in ignore})
 assert all(all(x==values[0]for x in values)for values in groups.values())
ignore={'cpu_seconds','init_seconds','run_seconds','total_seconds','peak_rss_bytes','variant','repeat','load'}
for i in range(-1,3):
 pair=[{k:v for k,v in r.items()if k not in ignore}for r in coupled if r['repeat']==i];assert len(pair)==2 and pair[0]==pair[1]
# Record experiment-only patches; none is applied to production.
f='src/engine/2d/solver/ExplicitInertialSolver.cpp'
import difflib
for v in ['limiter','friction','clear','combined','extrema']:
 a=(p/'src_baseline'/f).read_text().splitlines(True);b=(p/f'src_{v}'/f).read_text().splitlines(True)
 (p/f'rejected_{v}.patch').write_text(''.join(difflib.unified_diff(a,b,fromfile='a/'+f,tofile='b/'+f)))
(p/'retained.patch').write_text('')
loads=[x['load'][0]for x in screen+confirm+coupled]
verification=dict(baseline_commit=(p/'baseline_commit.txt').read_text().strip(),decision='No production changes retained',production_files_unchanged=manifest,screen_executions=len(screen),confirmation_executions=len(confirm),coupled_executions=len(coupled),measured_screen_executions=sum(r['repeat']>=0 for r in screen),measured_confirmation_executions=sum(r['repeat']>=0 for r in confirm),measured_coupled_executions=sum(r['repeat']>=0 for r in coupled),scalar_comparisons_exact=True,coupled_state_reports_and_hdf5_exact=True,host_one_minute_load_range=[min(loads),max(loads)],artifacts={f:hashlib.sha256((p/f).read_bytes()).hexdigest()for f in ['profiles.json','screen.json','screen_summary.json','confirmation.json','confirmation_summary.json','coupled_probe.json','coupled_probe_summary.json']})
(p/'final_verification.json').write_text(json.dumps(verification,indent=2))
labels={'limiter':'Skip nonbinding limiter divisions','friction':'Skip zero-roughness friction','clear':'Avoid duplicate gradient clearing','combined':'Combine those three shortcuts','extrema':'Reduce face extrapolations to extrema'}
lines=['| Prototype | Planar triangles, 1 thread | Planar quads, 1 thread | Radial quads, 1 thread | Planar quads, 4 threads |','|---|---:|---:|---:|---:|']
for v,label in labels.items():
 values=[]
 for case,q,t in [('planar',0,1),('planar',1,1),('radial',1,1),('planar',1,4)]:
  r=next(x for x in ss if x['variant']==v and (x['case'],x['quad'],x['threads'])==(case,q,t));values.append(f"{100*(1-r['median_cpu_ratio']):+.1f}%")
 lines.append('| '+label+' | '+' | '.join(values)+' |')
ct=['| Confirmation case | Median CPU reduction | Range across paired repeats |','|---|---:|---:|']
for r in cs:
 values=[100*(1-x)for x in r['ratios']];ct.append(f"| Planar {'triangles' if r['quad']==0 else 'quads'}, {r['threads']} thread(s) | {100*(1-r['median_cpu_ratio']):+.2f}% | {min(values):+.1f}% to {max(values):+.1f}% |")
values=[100*(1-x)for x in cp['ratios']];ct.append(f"| Wet 32,768-cell coupled surface, 1 thread | {100*(1-cp['median_cpu_ratio']):+.2f}% | {min(values):+.1f}% to {max(values):+.1f}% |")
text=f'''# Phase 11: remaining gradient and cell-update costs

## Decision

**Retain no production change.** Five isolated prototypes did not establish a substantial, consistent CPU improvement over phase 10. The strongest candidate, limiting once per variable using face-extrapolation extrema, improved one larger triangular case by a median 2.5%, but quad, four-thread and frictional complete-model comparisons were nearly unchanged. Those results do not justify replacing the current implementation for this performance objective.

The solver remains byte-for-byte identical to the phase-11 starting working tree. The pre-existing rain-activation edit remains untouched. This phase leaves only review artifacts and rejected experimental patches. **One phase remains: phase 12, final cumulative qualification.**

## Fresh baseline and profile

Baseline commit: `{verification['baseline_commit']}`. Exact current source hashes are recorded in `baseline_manifest.json`; isolated builds use that implementation and the existing review harness. No earlier timing result was used to accept a prototype.

Fresh wall-clock profiles of 64-division, three-period planar/radial bowls placed gradient reconstruction at roughly 23–28% of advance time and cell updates at 18–20%. Face evaluation excluding gradients still accounted for roughly 38–44%. These profiles were taken under heavy host load and guide candidate selection; they are not uncontended CPU-cost measurements.

The initial 64-division screening job was interrupted after completed groups when host contention made it too expensive for exploratory screening. `screen64_interrupted.*` preserves those partial observations, which are excluded from the decision tables below. Screening restarted at 32 divisions. Larger confirmations then used 64 divisions.

## Prototypes evaluated

- **Skip nonbinding limiter divisions:** avoid evaluating a ratio when the face extrapolation already lies within its neighbor bounds.
- **Skip zero-roughness friction:** omit the positive-depth friction calculation when Manning roughness is exactly zero. This specifically targets friction-free analytical problems; it does not remove friction from other cases.
- **Avoid duplicate gradient clearing:** zero gradients only on first-order fallback exits rather than zeroing and then overwriting them for reconstructed cells.
- **Combined shortcuts:** test the interaction of the preceding three changes.
- **Face-extrema limiter:** preserve the rounded face extrapolations, collect their positive/negative extrema, then calculate the final limiting factor with at most two relevant ratios per variable.

All prototypes stayed in review-local source trees. The original numerical thresholds, reconstruction formulas, pressure source, wet/dry rules, timestep controls and compiler floating-point settings were preserved. The accepted production code was never edited.

## Screening results

32-division bowls, three periods. One warm-up group followed by three randomized measured groups per case, comparing the baseline and all five prototypes. CPU time is child-process user plus system time, including setup and final scalar error evaluation. Entries are median paired CPU reductions: positive is faster; negative is slower.

{chr(10).join(lines)}

No shortcut showed a convincing general win. For example, delayed gradient clearing improved the triangular screen but worsened the quad screen, and the combined version did not reliably improve either. The friction shortcut helped one radial screen but was effectively unchanged elsewhere.

## Larger and frictional confirmations

The face-extrema candidate received further evaluation rather than being accepted from the small radial-screen gain. The 64-division planar cases run one period, with one warm-up pair and five randomized measured pairs. The complete-model probe runs a fully wet 32,768-triangle sloping surface for 20 seconds, including nonzero roughness, rainfall, infiltration, a drain/pipe connection and five-second HDF5 output. It uses one warm-up pair and three randomized measured pairs. This is an authored qualification fixture, not a calibrated catchment.

{chr(10).join(ct)}

The frictional complete-model result is inconclusive: paired changes range from about a 9.5% slowdown to a 6.6% improvement, with a median CPU reduction of only 0.52%. One-minute host load ranged from {min(loads):.1f} to {max(loads):.1f} over recorded screening and confirmation runs. Randomization and CPU-time accounting reduce some timing confounding but do not remove core placement, frequency, cache and scheduling effects. These experiments do not prove small improvements are impossible on an idle machine or other architectures; they do not provide evidence strong enough to retain this extra implementation complexity now.

## Numerical checks and scope

- **96 screening executions** and **36 larger-confirmation executions**: every recorded non-timing scalar metric matched the corresponding baseline exactly, including error measures, water-volume error, depth extrema and step counts. These comparisons are not claimed as full per-cell/history equality.
- **Eight complete-model executions**: final state hashes, water/species/groundwater ledger data as applicable, solver statistics, and normalized reports matched exactly. Native `h5diff` also found identical output datasets in every paired run, including warm-up.
- These are targeted prototype checks. No prototype was accepted, so the broader analytical matrix and full regression suite were not rerun in this phase. The production solver and existing tests are unchanged from the already validated phase-10 implementation. Phase 12 will perform final cumulative qualification.
- The component build uses native Apple ARM64 Clang, double precision, `-O3 -fno-fast-math -ffp-contract=off -mcpu=native`. Full-engine comparisons use the same internally consistent frozen support objects and headers on both sides, with only the candidate solver recompiled. No GPU experiment was run.

## Evidence

`profiles.json`, `screen.json`, `screen_summary.json`, `confirmation.json`, `confirmation_summary.json`, `coupled_probe.json`, `coupled_probe_summary.json`, `gradient_probe_provenance.json` and `final_verification.json` record the observations. `rejected_*.patch` preserves every prototype. `retained.patch` is empty. Binaries, full source snapshots and model outputs remain ignored local artifacts.

The next step is the final cumulative performance/accuracy assessment and supported-configuration review. No additional optimization phase is proposed on the evidence here.
'''
(p/'REPORT.md').write_text(text)
print(json.dumps(verification,indent=2))
