#!/usr/bin/env python3
"""Produce the review tables, figures and Markdown from a coupling-suite run.

Requires matplotlib and numpy. Run the native sweep first; no model is run or
calibrated by this script. Numerical gates and observed fit remain distinct.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

REF = Path(__file__).resolve().parent
COLORS = ['#34495e', '#b45438', '#007c91']


def rows(path):
    with path.open(newline='') as f:
        return list(csv.DictReader(f))


def equation(row):
    hs = float(row['h_s_m'] or 0)
    hp = float(row['h_p_m'] or 0)
    k = math.sqrt(2*9.80665)
    if row['scenario'] == 'S1':
        return 2/3 * math.pi*.24*k*hs**1.5
    if row['scenario'] == 'S2':
        return math.pi*.24*k*hs*math.sqrt(hs+.478-hp)
    return math.pi*.24**2/4*k*math.sqrt(hp-hs-.478)


def table(headers, data):
    return '\n'.join(['| '+' | '.join(headers)+' |',
                      '| '+' | '.join('---' for _ in headers)+' |']+
                     ['| '+' | '.join(str(x) for x in row)+' |' for row in data])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--run-dir', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--refinement', type=Path)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    run = args.run_dir.resolve()
    scores = json.loads((run/'scores.json').read_text())
    ref = rows(REF/'table2_steady_exchange.csv')
    coeffs = {r['scenario']: float(r['c_expected']) for r in rows(REF/'table3_coefficients.csv')}
    model = rows(run/'rubinato2017_model_out/model_vs_table2.csv')
    shaft = rows(run/'rubinato2017_model_out/shaft_vs_table2.csv')
    fine = rows(run/'rubinato2017_model_out/fine_mesh_equalisation.csv')
    stats, regression, comparison = {}, [], []
    for scenario in ('S1','S2','S3','S4'):
        rr = [r for r in ref if r['scenario'] == scenario]
        obs = np.array([float(r['q_e_lps']) for r in rr])
        xi = np.array([float(r['xi_f_lps']) for r in rr])
        x = np.array([equation(r) for r in rr])
        slope = float(np.dot(x-x.mean(), obs/1000-obs.mean()/1000)/np.dot(x-x.mean(),x-x.mean()))
        intercept = float(obs.mean()-1000*slope*x.mean())
        r2 = float(np.corrcoef(x,obs)[0,1]**2)
        published = 1000*coeffs[scenario]*x
        regression.append(dict(scenario=scenario, n=len(rr), c_table3=coeffs[scenario],
            slope=slope, intercept_lps=intercept, r2=r2,
            zero_intercept_coefficient=float(np.dot(x,obs/1000)/np.dot(x,x))))
        stats[scenario] = {}
        for label, data in [('paper_equation', published),
                            ('junction', [abs(float(r['q_num_lps'])) for r in model
                                          if r['scenario']==scenario and r['law']=='WEIR_ORIFICE']),
                            ('shaft', [abs(float(r['q_num_lps'])) for r in shaft if r['scenario']==scenario])]:
            data = np.asarray(data)
            if len(data) != len(obs):
                raise ValueError(f'{scenario} {label}: incomplete sweep')
            bias = 100*(data/obs-1)
            stats[scenario][label] = dict(n=len(obs), min_bias_pct=float(bias.min()),
                max_bias_pct=float(bias.max()), median_bias_pct=float(np.median(bias)),
                mean_abs_residual_lps=float(np.mean(abs(data-obs))),
                within_xi_plus_2pct=int(np.sum(abs(data-obs)<=xi+0.02*obs)))
        for i,r in enumerate(rr):
            a = [v for v in model if v['scenario']==scenario and v['law']=='WEIR_ORIFICE'][i]
            b = [v for v in shaft if v['scenario']==scenario][i]
            comparison.append(dict(case=f'{scenario}_{i+1}', observed_lps=obs[i], xi_f_lps=xi[i],
                paper_equation_lps=published[i], junction_lps=abs(float(a['q_num_lps'])),
                shaft_lps=abs(float(b['q_num_lps'])), junction_continuity_pct=float(a['continuity_pct']),
                shaft_continuity_pct=float(b['continuity_pct']), shaft_surface_depth_m=float(b['surface_depth_m']),
                shaft_node_head_m=float(b['node_head_m']), h_s_m=r['h_s_m'], h_p_m=r['h_p_m']))
    with (out/'steady_comparison.csv').open('w',newline='') as f:
        writer = csv.DictWriter(f, fieldnames=list(comparison[0]))
        writer.writeheader(); writer.writerows(comparison)
    synthetic = [c for c in scores['cells'] if c['case'].startswith('synthetic-')]
    if len(synthetic) != 27:
        raise ValueError('Expected 9 events times 3 coefficient presets')
    if any(c['verdict']=='ERROR' for c in synthetic):
        raise ValueError('Synthetic sweep has execution errors')
    summary = dict(stats=stats, regression=regression, fine_mesh=fine,
                   scores_summary={v:sum(c['verdict']==v for c in scores['cells'])
                                   for v in ('PASS','FAIL','ERROR','UNAVAILABLE')},
                   synthetic_passed=sum(c['verdict']=='PASS' for c in synthetic),
                   synthetic_continuity_range=[min(c['continuity_err'] for c in synthetic),
                                               max(c['continuity_err'] for c in synthetic)],
                   engine_sha=scores['engine_sha'],
                   binary_manifest=json.loads((run/'binary_manifest.json').read_text()))
    refinement = json.loads(args.refinement.read_text()) if args.refinement else []
    if refinement:
        summary['routing_refinement'] = refinement
    (out/'results_summary.json').write_text(json.dumps(summary, indent=2, allow_nan=False))

    plt.rcParams.update({'font.size':9,'axes.spines.top':False,'axes.spines.right':False,
                         'axes.titleweight':'bold','savefig.dpi':180})
    fig,axes=plt.subplots(2,2,figsize=(8,5.4),constrained_layout=True)
    for ax,s in zip(axes.flat,stats):
        r=[v for v in comparison if v['case'].startswith(s+'_')]
        x=np.array([v['observed_lps'] for v in r])
        maximum=max(max(v[k] for v in r) for k in ['observed_lps','paper_equation_lps','junction_lps','shaft_lps'])*1.08
        ax.plot([0,maximum],[0,maximum],color='#777777',ls='--',lw=1,label='1:1')
        for key,color,marker,label in zip(['paper_equation_lps','junction_lps','shaft_lps'],COLORS,
                ['x','o','s'],['Paper equation at measured heads','Junction reconstruction','Physical shaft sensitivity']):
            ax.scatter(x,[v[key] for v in r],s=24,c=color,marker=marker,label=label)
        ax.errorbar(x,x,xerr=[v['xi_f_lps'] for v in r],fmt='none',ecolor='#999999',lw=.8)
        ax.set(xlim=(0,maximum),ylim=(0,maximum),title=s,xlabel='Measured exchange magnitude (L/s)',
               ylabel='Calculated magnitude (L/s)')
        ax.grid(alpha=.15)
    handles,labels=axes[0,0].get_legend_handles_labels()
    fig.legend(handles,labels,loc='outside lower center',ncol=2,frameon=False)
    fig.savefig(out/'steady_fit.png');plt.close(fig)
    fig,axes=plt.subplots(1,2,figsize=(8,3.1),constrained_layout=True)
    for j,s in enumerate(stats):
        data=[r for r in comparison if r['case'].startswith(s+'_')]
        for key,color,offset,label in [('junction',COLORS[1],-.09,'Junction'),('shaft',COLORS[2],.09,'Physical shaft')]:
            axes[0].scatter([j+offset]*len(data),[r[key+'_continuity_pct'] for r in data],s=17,color=color,label=label if j==0 else None)
            axes[1].scatter([j+offset]*len(data),[100*(r[key+'_lps']/r['observed_lps']-1) for r in data],s=17,color=color)
    for ax in axes:
        ax.set_xticks(range(4),stats.keys());ax.axhline(0,color='#999999',lw=.7);ax.grid(axis='y',alpha=.15)
    axes[0].axhspan(-2,2,color='#007c91',alpha=.1)
    axes[0].set(ylabel='1D routing continuity error (%)',title='Book actual shaft storage')
    axes[1].set(ylabel='Exchange residual / measurement (%)',title='Agreement remains incomplete')
    axes[0].legend(frameon=False,fontsize=8)
    fig.savefig(out/'model_diagnostics.png');plt.close(fig)
    fig,axes=plt.subplots(3,1,figsize=(8,4.8),sharex=True,constrained_layout=True)
    for ax,event in zip(axes,['T1','T5','T9']):
        curves={}
        for bound in ['lower','expected','upper']:
            data=rows(run/f'synthetic-{event}-{bound}/metrics.csv')
            curves[bound]=(np.array([float(r['time_s']) for r in data]),np.array([float(r['q_to_surface_lps']) for r in data]))
        t,q=curves['expected']
        ys=np.array([np.interp(t,*curves[b]) for b in curves])
        ax.fill_between(t,ys.min(axis=0),ys.max(axis=0),color='#007c91',alpha=.2,label='Preset envelope')
        ax.plot(t,q,color='#007c91',lw=1,label='Expected preset')
        ax.axhline(0,color='#777777',lw=.6);ax.grid(alpha=.15)
        ax.set(ylabel='To surface (L/s)',title=f'Synthetic {event} constraints; no measured curve')
    axes[-1].set_xlabel('Elapsed time (s)');axes[0].legend(loc='upper right',frameon=False)
    fig.savefig(out/'synthetic_events.png');plt.close(fig)

    bias_table=table(['Scenario / n','Paper eq. bias (%)','Junction bias (%)','Shaft bias (%)'],[
        [f'{s} / {stats[s]["shaft"]["n"]}']+
        [f'{stats[s][k]["min_bias_pct"]:+.1f} to {stats[s][k]["max_bias_pct"]:+.1f}'
         for k in ['paper_equation','junction','shaft']] for s in stats])
    fit_table=table(['Scenario','Paper C','Recovered slope','Intercept (L/s)','R-squared'],[
        [r['scenario'],f'{r["c_table3"]:.3f}',f'{r["slope"]:.4f}',f'{r["intercept_lps"]:+.3f}',f'{r["r2"]:.4f}'] for r in regression])
    diagnostics=[]
    for s in stats:
        cells=[c for c in scores['cells'] if c['case'].startswith(s+'_') and c.get('model_variant')=='physical_shaft']
        diagnostics.append([s,f'{min(c["continuity_err"] for c in cells):+.2f} to {max(c["continuity_err"] for c in cells):+.2f}',
            f'{max(c["steady_drift_pct"] for c in cells):.2f}',
            f'{sum(c["within_xi_plus_2pct"] for c in cells)}/{len(cells)}',
            f'{sum(c["verdict"]=="PASS" for c in cells)}/{len(cells)}'])
    diagnostic_table=table(['Scenario','Shaft continuity (%)','Max drift (%)','Within xi + 2%','Numerical gates'],diagnostics)
    refinement_text = ''
    if refinement:
        refinement_text = '\n\n## Routing-step sensitivity\n\n' + table(
            ['Selected case','Q at 0.5 s (L/s)','Q at 0.05 s (L/s)','Drift at 0.05 s (%)'],
            [[r['case'],f'{r["coarse_q_lps"]:+.3f}',f'{r["fine_q_lps"]:+.3f}',f'{r["fine_drift_pct"]:.5f}']
             for r in refinement]) + ('\n\nOne case per scenario was selected by the largest 0.5 s window drift. '
            'Reducing the routing step stabilises those windows but changes discharge materially. '
            'Some apparent coarse-step agreement with measurements is therefore unreliable. '
            'This two-step sensitivity is not a time-convergence study; the full steady matrix above remains '
            'a 0.5 s diagnostic. Signed rates here are positive to the surface.\n')
    event_table=[]
    for event in rows(REF/'table1_unsteady_tests.csv'):
        cells=[c for c in synthetic if c['case'].split('-')[1]==event['test_id']]
        expected=next(c['metrics'] for c in cells if c['case'].endswith('expected'))
        event_table.append([event['test_id'],event['q3max_over_q1'],event['duration_s'],f'{expected["spill_m3"]:.3f}',
            f'{expected["drain_m3"]:.3f}',f'{min(c["continuity_err"] for c in cells):.2f} to {max(c["continuity_err"] for c in cells):.2f}'])
    event_table=table(['Event','Peak Q3/Q1','Plateau (s)','Spill (m3)','Drain (m3)','Continuity (%)'],event_table)
    paper_pass=sum(stats[s]['paper_equation']['within_xi_plus_2pct'] for s in stats)
    shaft_pass=sum(stats[s]['shaft']['within_xi_plus_2pct'] for s in stats)
    cap_ratio=float(fine[0]['q_num_lps'])/float(fine[1]['q_num_lps'])
    md=f'''# Fine-scale 1D/2D coupling: review of Rubinato et al. (2017)

**Review draft - 21 October 2026. Experimental qualification is incomplete.**

The implemented coupling features pass their focused engine checks. The comparison with the laboratory measurements does not yet support calling the reconstructed OpenSWMM flume experimentally validated. This report retains the discrepancies, separates numerical checks from measured agreement, and provides draft material for the hydraulic reference manual and SWMMVis example documentation. Publication in those manuals is pending review.

## Outcome at a glance

{bias_table}

Bias is 100 (calculated magnitude / measured magnitude - 1). Each range covers every Table 2 row in that scenario. These are goodness-of-fit diagnostics on the calibration measurements, not independent validation or numerical convergence estimates.
The full-engine table uses a 0.5 s routing step; the representative 0.05 s checks below show that it is not a converged experimental comparison. The broader suite records **{summary['scores_summary']['PASS']} numerical passes and {summary['scores_summary']['FAIL']} numerical failures**, with no execution errors. Measured-flow agreement is reported separately.

## What was already complete

- Engine: shared CPU/device exchange law; free/submerged weir and orifice regimes; coefficient overrides; surface-head options; conservative exchange booking; pond modes and equalisation; outfall tailwater, spreading and reporting; node attributes, INP/GeoPackage and C/Python access.
- SWMMVis: node coupling edits and selector, meshing eligibility, remap preview/undo, unauthored map coefficients, options and INP persistence. The prior handoff still lists map styling by eligibility and per-node mapper-law precedence as follow-ups.
- Earlier evidence: Tables 1-3 transcribed, law regression and reconstructed steady flume tests, a fine-mesh anti-choking check, and churn comparisons. The standalone benchmark decks lacked metadata and a runnable coupling suite.

## Completed in this review

The benchmark now has six validated metadata/provenance records and a runnable coupling suite. Run completion, head samples, two successive steady windows and continuity are exported. A physical-shaft sensitivity explicitly books the 0.04524 m2 shaft volume and uses that area as MIN_SURFAREA; the former numerical floor was about 1.167 m2, or 25.8 shaft areas. No discharge coefficient was retuned.

Two reconstruction errors were corrected: Q3 is now obtained from Table 2's definition of surcharge percentage, rather than identifying pipe Reynolds flow as inlet flow; and synthetic timestamps now use hours:minutes:seconds, placing the pulse within the 600 s run. The synthetic example also uses actual shaft storage and an explicitly assumed 0.45 m downstream stage so that both transfer directions occur.

<!-- pagebreak -->

## 1. What the paper measures

The no-lid rig has a 0.24 m manhole, a 0.075 m pipe, an 8 m by 4 m flume, slope 0.001, Manning n = 0.009 and crest 0.478 m above the pipe invert. S1 is free-weir drainage into an unsurcharged sewer (15 rows); S2 is submerged-weir drainage (5); S3 is sewer discharge onto a wet surface (8); S4 is sewer discharge with no incoming surface flow (8). S4 has no tabulated surface head. Equation 3's surface-to-sewer orifice coefficient was not calibrated by these experiments.

For this report, L = pi D, A = pi D squared / 4 and all heads share the pipe-invert datum. The isolated paper terms are X1 = (2/3) L sqrt(2g) hs to the power 3/2; X2 = L sqrt(2g) hs sqrt(hs + z - hp); X3/X4 = A sqrt(2g) sqrt(hp - hs - z). Calculated magnitude is C X. This calculation has no smoothing, availability cap or modelled surface response.

{fit_table}

The recovered slopes are from ordinary fits Q = C X + b using the rounded Table 2 data. The paper's Figures 6-9 display nonzero intercepts; Table 3 gives their slopes. In particular, the S3/S4 intercepts are large relative to the measured flows. High R-squared and matching slopes therefore do not establish absolute discharge agreement: the bare zero-intercept paper equations meet xi_f + 2% of measured flow in **{paper_pass}/36** rows. The corresponding physical-shaft runs meet that illustrative band in **{shaft_pass}/36** rows. The band is a comparison criterion, not a statistical confidence interval.

![Steady comparison](steady_fit.png)

Horizontal bars show the paper's xi_f instrument/balance variation. Each panel uses its own scale. The dashed line is equality. Paper equation points use measured heads; the other points use independently evolved surface heads and reconstructed boundary conditions.

<!-- pagebreak -->

## 2. Full-engine reconstruction and conservation

Both steady variants use 1,600 triangular cells at nominal 0.2 m spacing, dynamic-wave routing at 0.5 s, a central manhole, STENCIL_WET_MEAN and the RUBINATO2017 preset. They run 300 s from a dry start. Exchange is the cumulative transfer difference over 240-300 s, compared with 180-240 s for drift. A short, wide downstream pipe imposes the measured sewer stage approximately; this is an artificial head reservoir, not the laboratory's complete sewer system.

Surface inflow is reconstructed as Q1 = Fr sqrt(g hs) hs W, assuming representative uniform velocity across the 4 m width. S4 has Q1 = 0. Sewer inflow is Q3 = absolute(Qe / (percentage / 100)) for S2-S4, and zero for S1. These inputs reuse the measured calibration rows, so the exercise is not a blind prediction. Fr and Reynolds entries contain apparent inconsistencies in the article and should not be silently 'corrected'.

{diagnostic_table}

Numerical gates require completion, correct exchange direction, at most 2% successive-window drift and at most 2% absolute ordinary 1D routing continuity error. Observed discharge fit remains report-only. The engine's historical unit test only guarded direction and severe choking; it was never an experimental accuracy gate.

![Model diagnostics](model_diagnostics.png)

The shaft variant changes the storage representation and the numerical area floor together; the improvement cannot be assigned to either change alone. It improves conservation substantially, but residual continuity or drift failures remain visible. Continuity is the ordinary 1D ledger over the whole run, not a coupling-only error or a combined-domain balance.
{refinement_text}

<!-- pagebreak -->

## 3. Unsteady results: synthetic demonstration, not measured validation

The corrected demonstration uses the nine Table 1 peak ratios and durations, constant surface inflow 8.15 L/s, a 30 s rising ramp beginning at 60 s, a plateau of the stated duration and a 30 s falling ramp. All runs last 600 s. The paper instead used measured sewer pressure at each timestep and evaluated 500 s events. Matching a peak ratio and duration does not recreate those pressure histories or measured exchange series.

{event_table}

Spill/drain columns use the expected preset. Continuity ranges cover lower/expected/upper presets; **{summary['synthetic_passed']}/27** runs pass the synthetic checks, including nonnegative depth, both transfer directions, at least two reversals and the 2% 1D continuity criterion. Signed net transfer may be negative even when substantial surcharge occurs; gross spill and drain must be reported separately.

![Synthetic exchange events](synthetic_events.png)

Curves are OpenSWMM output only. Shading is the envelope of the three coefficient presets, not experimental uncertainty. No observed transient curve is plotted. The source paper reports excess numerical surcharge and sensitivity to head sampling, but that qualitative finding cannot validate this different forcing. Its HMho approach generally improves longer-event agreement; the text also reports shorter events for which other head definitions are closer. The end-of-surcharge suction oscillation requires physics beyond this depth-averaged coupling model.

<!-- pagebreak -->

## 4. Fine mesh, limitations and publication decision

The existing fine-mesh S3 check has **{int(fine[0]['cells']):,} cells**, with 0.0125 m grid spacing near the manhole, rather than the paper's 11,186-cell mesh. Equalisation on/off gives **{float(fine[0]['q_num_lps']):.4f} / {float(fine[1]['q_num_lps']):.4f} L/s**, ratio **{cap_ratio:.4f}**. It passes the anti-choking criterion; this does not demonstrate spatial convergence. The junction exchange is concentrated into one delivery cell even when its opening covers many cells. Its fine-mesh continuity is **{float(fine[0]['continuity_pct']):.2f}%** and is not hidden by the anti-choking result.

The law-only regression uses a lab-scaled square-root regularisation head of 0.02/6 m. The routed model retains the engine's 0.02 m regularisation, inside the measured millimetre-scale head range. The preset also uses COO = 0.160 for both wet and dry outward flow, whereas the paper's wet S3 slope is 0.167; COI = 0.167 is borrowed for inward orifice flow and is not calibrated by this paper. Near the S2 crest, blending and millimetre-rounded pressure values further affect comparisons. No compensating intercept or new coefficient fit has been added to hide these differences.

Focused verification: **93 tests in 16 engine programs passed**, followed by the new physical-shaft test. Five of six GUI programs passed against the existing private 1D/2D install; mesh-engine sync reproduced its two previously reported fixture initialisation failures. These GUI checks use that earlier engine install, not the pinned report binary. GPU runtime accuracy, TRACKING restart support, map styling and per-node mapper precedence are not certified by this CPU/laboratory report.

The exported surface probe is the nearest centroid to the assumed point 0.46 m upstream of the central manhole; on the coarse mesh it is at (3.5333, 2.0667) m, rather than the exact transducer position. Its depth and the modelled manhole head are in steady_comparison.csv. Table 2's pipe pressure was measured 0.35 m away; prescribing it as manhole head omits those local losses. STENCIL_WET_MEAN also does not reproduce the paper's average over every cell inside the circular opening.

## Required before an experimental validation claim

1. Obtain and archive the measured transient sewer-head and exchange time series, or make a separately reviewed digitisation of Figures 5, 10 and 11 with uncertainty and event identification. The DOI supplementary endpoint was not retrievable in this review; the Sheffield project outputs page does not expose an associated 2017 data link. This is not evidence that raw data do not exist.
2. Resolve all physical-shaft continuity/steadiness failures; demonstrate routing-step and mesh refinement while sampling the actual transducer position and opening footprint. Compare both domain ledgers and the combined balance.
3. Review the lab-scale regularisation, head sampling, local pipe losses and single-cell delivery assumptions before changing runtime physics. Any coefficient recalibration on Table 2 must be labelled calibration and evaluated on independent measured events.

**Publication recommendation:** retain the report and runnable example as a qualified comparison and numerical demonstration. The manual drafts must carry the limitations and measured-fit table. Do not describe the case as an experimentally validated 1D/2D solver example. Manual navigation and published pages have not been changed; review the accompanying drafts first.

## Reproducibility and evidence

The benchmark suite output includes every generated deck, report, transient series, steady CSV, numerical gate result, and a SHA-256 binary manifest. The recorded engine identifier is `{scores['engine_sha']}`; it denotes a dirty shared checkout, not a clean release. The executable/library hashes identify the actual pinned binaries, which were retained locally to isolate this sweep from concurrent engine work. The report's results_summary.json contains those hashes and the complete scenario summaries. steady_comparison.csv preserves all 36 measured rows and both full-engine variants.

Run the native test build, then the benchmarks entry point with --suite coupling, --native-bin pointing at its bin directory and --library pointing at the engine shared library. Set --output-dir to a new review directory. A nonzero suite status reports numerical gate failures even though observed fit is report-only. Regenerate this report with tests/benchmarks/rubinato2017/summarize.py --run-dir [suite output] --output [report folder]. See the benchmark suite README for complete commands and prerequisites.

Source: Rubinato, M., Martins, R., Kesserwani, G., Leandro, J., Djordjevic, S., and Shucksmith, J. (2017). Experimental calibration and validation of sewer/surface flow exchange equations in steady and unsteady flow conditions. Journal of Hydrology 552, 421-432. DOI: https://doi.org/10.1016/j.jhydrol.2017.06.024. Tables 1-3; Figures 2, 6-11; Sections 3.1, 3.2.1 and 4.4. Source tables are CC-BY-4.0; original numerical decks are CC0-1.0. Data availability check: https://sheffield.ac.uk/floodinteract/outputs (21 October 2026).
'''
    (out/'review.md').write_text(md)
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
