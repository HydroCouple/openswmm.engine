"""Plot archived measurements and numerical outputs; no model execution.

The figure pipeline's synthetic tier denotes a numpy/matplotlib-only build,
not the origin of observations. Table 2 points are experimental; T9 is synthetic.
Rebuild the snapshot with tests/benchmarks/rubinato2017/build_visual_data.py.
"""
import json
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Circle, Patch, Rectangle
import numpy as np

REQUIRES = ()
DATA = Path(__file__).resolve().parents[2]/'data/rubinato2017.json'
BLUE, ORANGE, GRAY, INK = '#176bba', '#c76024', '#86939c', '#193447'
REGIMES = [('S1', 'Free-weir drainage'), ('S2', 'Submerged-weir drainage'),
           ('S3', 'Surcharge onto wet surface'), ('S4', 'No incoming surface flow')]
SERIES = [('junction_lps', GRAY, '^', 'Junction reconstruction'),
          ('paper_equation_lps', ORANGE, 'x', 'Paper equation at measured heads'),
          ('shaft_lps', BLUE, 'o', 'Physical-shaft reconstruction')]


def frame(title, subtitle, height=7.1):
    fig = plt.figure(figsize=(9.8, height), facecolor='#fcfcfb')
    # Preserve an outer margin when the common sink exports bbox_inches='tight'.
    fig.add_artist(Rectangle((0, 0), 1, 1, transform=fig.transFigure,
                             fc='none', ec='none'))
    fig.text(.07, .97, title, fontsize=18, weight='bold', color=INK, va='top')
    fig.text(.07, .925, subtitle, fontsize=9, color='#576976', va='top')
    return fig


def chrome(ax):
    import style
    style.style_ax(ax)
    ax.tick_params(colors='#576976')
    ax.xaxis.label.set_color(INK)
    ax.yaxis.label.set_color(INK)


def legend(fig, extras=()):
    handles = [Line2D([], [], color=c, marker=m, ls='none', markersize=6,
                      markerfacecolor='none' if k == 'junction_lps' else c, label=lab)
               for k, c, m, lab in SERIES]
    fig.legend(handles=handles+list(extras), loc='lower center', bbox_to_anchor=(.5, .015),
               ncol=2, fontsize=8, frameon=False)


def parity(data):
    fig = frame('Experimental versus simulated exchange',
                '36 published steady measurements • reconstructed flume • 0.5 s routing step; convergence unresolved', 8.3)
    axes = fig.subplots(2, 2)
    fig.subplots_adjust(left=.09, right=.96, top=.835, bottom=.15, hspace=.65, wspace=.4)
    for ax, (s, regime) in zip(axes.flat, REGIMES):
        rr = [r for r in data['steady'] if r['case'].startswith(s+'_')]
        x = np.array([r['observed_lps'] for r in rr])
        maximum = max(r[k] for r in rr for k in ['observed_lps']+[v[0] for v in SERIES])*1.09
        ax.plot([0, maximum], [0, maximum], color=GRAY, ls='--', lw=1)
        for key, color, marker, _ in SERIES:
            y = [r[key] for r in rr]
            ax.scatter(x, y, color=color, marker=marker, s=29,
                       facecolors='none' if key == 'junction_lps' else color, zorder=3)
        ax.errorbar(x, [r['shaft_lps'] for r in rr], xerr=[r['xi_f_lps'] for r in rr],
                    fmt='none', ecolor=BLUE, alpha=.6, lw=.8, capsize=2)
        mae = np.mean([abs(r['shaft_lps']-r['observed_lps']) for r in rr])
        ax.set(xlim=(0, maximum), ylim=(0, maximum),
               xlabel='Experimental |Q| (L/s)', ylabel='Calculated |Q| (L/s)')
        ax.set_title(f'{s}  {regime}\nn = {len(rr)}; shaft MAE = {mae:.3f} L/s',
                     loc='left', fontsize=9, pad=10)
        ax.set_aspect('equal', adjustable='box')
        chrome(ax)
    legend(fig, [Line2D([], [], color=GRAY, ls='--', label='1:1 agreement')])
    return fig


def residuals(data):
    fig = frame('Where the exchange predictions differ',
                'Residual = calculated magnitude − experimental magnitude • same 36 steady rows; no coefficient retuning')
    axes = fig.subplots(2, 2)
    fig.subplots_adjust(left=.09, right=.96, top=.835, bottom=.15, hspace=.65, wspace=.4)
    for ax, (s, regime) in zip(axes.flat, REGIMES):
        rr = [r for r in data['steady'] if r['case'].startswith(s+'_')]
        x = np.arange(1, len(rr)+1)
        obs = np.array([r['observed_lps'] for r in rr])
        band = np.array([r['xi_f_lps'] for r in rr])+.02*obs
        ax.fill_between(x, -band, band, color='#cfe6d8', alpha=.8)
        ax.axhline(0, color=GRAY, lw=.8)
        for key, color, marker, _ in SERIES:
            ax.scatter(x, [r[key] for r in rr]-obs, color=color, marker=marker, s=25,
                       facecolors='none' if key == 'junction_lps' else color, zorder=3)
        count = sum(abs(r['shaft_lps']-r['observed_lps']) <= b for r, b in zip(rr, band))
        ax.set_title(f'{s}  {regime}\nShaft: {count}/{len(rr)} within illustrative band',
                     loc='left', fontsize=10, pad=9)
        ax.set(xlabel=f'Table 2 row within {s}', ylabel='Residual (L/s)', xlim=(.5, len(rr)+.5))
        ax.set_xticks(x if len(x) <= 8 else [1, 3, 5, 7, 9, 11, 13, 15])
        chrome(ax)
    legend(fig, [Patch(facecolor='#cfe6d8', label='±(ξf + 2% of measurement); not a confidence interval')])
    return fig


def sensitivity(data):
    fig = frame('Smaller routing steps change the answer',
                'One selected physical-shaft case per regime • signed exchange: positive to surface • two steps do not establish convergence', 5)
    axes = fig.subplots(1, 4)
    fig.subplots_adjust(left=.085, right=.98, top=.74, bottom=.26, wspace=.55)
    for ax, rr in zip(axes, data['refinement']):
        case = rr['case'].removesuffix('_SHAFT')
        r = next(v for v in data['steady'] if v['case'] == case)
        obs = r['observed_lps'] * (-1 if case[:2] in ('S1', 'S2') else 1)
        xi = r['xi_f_lps']
        ax.axhspan(obs-xi, obs+xi, color='#cfe6d8')
        ax.axhline(obs, color='#247249', lw=1.5)
        ys = [rr['coarse_q_lps'], rr['fine_q_lps']]
        ax.plot([0, 1], ys, color=BLUE, marker='o', lw=1.5)
        for x, y in enumerate(ys):
            ax.annotate(f'{y:+.3f}', (x, y), xytext=(0, 9 if y >= np.mean(ys) else -15),
                        textcoords='offset points', ha='center', fontsize=8, color=BLUE)
        low, high = min(ys+[obs-xi]), max(ys+[obs+xi])
        pad = max((high-low)*.45, .04)
        ax.set(xticks=[0, 1], xticklabels=['0.5 s', '0.05 s'], xlim=(-.35, 1.35),
               ylim=(low-pad, high+pad), xlabel='Routing step', title=case)
        chrome(ax)
    axes[0].set_ylabel('Signed exchange (L/s)')
    fig.legend(handles=[Line2D([], [], color=BLUE, marker='o', label='Simulated late-window mean'),
                        Line2D([], [], color='#247249', label='Experimental rate ± ξf')],
               loc='lower center', bbox_to_anchor=(.5, .04), ncol=2, frameon=False)
    return fig


def storyboard(data):
    t = data['transient']
    fig = frame('Follow surcharge and drainback across the flume',
                'Synthetic T9 • actual saved cell depths on 1,600 triangles • constant colour scale • no experimental transient series', 6.8)
    gs = fig.add_gridspec(2, 3, left=.07, right=.91, top=.82, bottom=.12,
                         height_ratios=[1, 1.05], hspace=.65, wspace=.22)
    nodes, tri = np.array(t['nodes']), np.array(t['triangles'])
    times = np.array(t['frame_times_s'])
    for j, (stamp, label) in enumerate([(30, 'Before the pulse'), (250, 'Surcharge plateau'), (500, 'After the pulse')]):
        ax = fig.add_subplot(gs[0, j]); i = int(np.argmin(abs(times-stamp)))
        mesh = ax.tripcolor(nodes[:, 0], nodes[:, 1], tri, facecolors=t['depths_mm'][i],
                            vmin=0, vmax=25, cmap='Blues', rasterized=True)
        ax.add_patch(Circle((4, 2), .12, fc='none', ec=ORANGE, lw=1.8))
        ax.set(aspect='equal', xlim=(0, 8), ylim=(0, 4), xlabel='Along flume (m)', yticks=[0, 2, 4])
        ax.set_title(f'{label}\n{times[i]:.0f} s', fontsize=10, loc='left', pad=10)
        if j == 0: ax.set_ylabel('Across flume (m)')
    cax = fig.add_axes([.93, .555, .015, .235])
    fig.colorbar(mesh, cax=cax, label='Cell depth (mm)', ticks=[0, 5, 10, 15, 20, 25])
    ax = fig.add_subplot(gs[1, :]); x, q = np.array(t['times_s']), np.array(t['q_lps'])
    ax.fill_between(x, 0, q, where=q >= 0, color=ORANGE, alpha=.4, label='Sewer → surface')
    ax.fill_between(x, 0, q, where=q < 0, color=BLUE, alpha=.4, label='Surface → sewer')
    ax.plot(x, q, color=INK, lw=1)
    for stamp in (30, 250, 500): ax.axvline(stamp, color=GRAY, ls=':', lw=1)
    ax.axhline(0, color=GRAY, lw=.8)
    ax.set(xlim=(0, 600), xlabel='Elapsed time (s)', ylabel='To surface (L/s)')
    chrome(ax); ax.legend(loc='lower center', ncol=2, fontsize=8)
    fig.text(.07, .865, 'Gross spill  1.415 m³     Gross drainback  0.201 m³     Net to surface  1.214 m³',
             fontsize=11, color=INK)
    fig.text(.07, .025, 'Orange ring: 0.24 m manhole. Saved frames every 10 s; brief peaks between frames are not shown.', fontsize=8, color='#576976')
    return fig


def build(sink):
    data = json.loads(DATA.read_text())
    for suffix, draw in [('parity', parity), ('residuals', residuals),
                         ('routing_steps', sensitivity), ('transient', storyboard)]:
        name = 'hydraulics_ch9_rubinato_'+suffix
        if name in sink.expected:
            sink.save(draw(data), name)
            svg = sink.svg_dir/(name+'.svg')
            svg.write_text('\n'.join(line.rstrip() for line in svg.read_text().splitlines())+'\n')
