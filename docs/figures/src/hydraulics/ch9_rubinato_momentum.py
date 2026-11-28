"""Plot the dated momentum study snapshot; experimental and numerical data remain distinct.

The figure pipeline's synthetic tier means a plotting-only generator, not
synthetic observations. Rebuild data with summarize_momentum.py after a native sweep.
"""
import json
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Rectangle
import numpy as np

REQUIRES = ()
DATA = Path(__file__).resolve().parents[2]/'data/rubinato2017_momentum.json'
BLUE, GRAY, ORANGE, GREEN = '#176bba', '#89969f', '#d47721', '#23724b'
REGIMES = [('S1', 'Free-weir drainage'), ('S2', 'Submerged-weir drainage'),
           ('S3', 'Surcharge onto wet surface'), ('S4', 'No incoming surface flow')]


def canvas(title, subtitle):
    fig, axes = plt.subplots(2, 2, figsize=(9.8, 8.4), facecolor='#fcfcfb')
    # The common exporter crops tightly with no padding; retain canvas margins.
    fig.add_artist(Rectangle((0, 0), 1, 1, transform=fig.transFigure, fc='none', ec='none'))
    fig.subplots_adjust(left=.11, right=.95, top=.81, bottom=.16, hspace=.74, wspace=.48)
    fig.text(.07, .965, title, fontsize=16, weight='bold', color='#193447', va='top')
    fig.text(.07, .91, subtitle, fontsize=8.5, color='#576976', va='top')
    for ax in axes.flat:
        ax.spines[['top', 'right']].set_visible(False)
        ax.grid(alpha=.15)
        ax.set_axisbelow(True)
    return fig, axes


def parity(data):
    fig, axes = canvas('Fine timesteps expose the remaining model bias',
        '36 measured steady rows • θ = 0.8 over a fixed 1 s interval • no coefficient retuning\n'
        'Blue: new 0.003125 s simulation; grey: archived 0.5 s per-step smoothing')
    for ax, (regime, label) in zip(axes.flat, REGIMES):
        rr = [r for r in data['rows'] if r['regime'] == regime]
        obs = np.array([r['observed_lps'] for r in rr])
        new = np.array([abs(r['q_lps'][-1]) for r in rr])
        old = np.array([r['legacy_magnitude_lps'] for r in rr])
        xi = np.array([r['xi_f_lps'] for r in rr])
        limit=max(max(obs+xi), max(new), max(old))*1.10
        ax.plot([0, limit], [0, limit], color=GREEN, ls='--', lw=1.2)
        for x, a, b in zip(obs, old, new):
            ax.plot([x, x], [a, b], color=GRAY, alpha=.32, lw=.9)
        ax.scatter(obs, old, marker='o', facecolors='none', edgecolors=GRAY, s=31, zorder=3)
        ax.errorbar(obs, new, xerr=xi, fmt='o', color=BLUE, markersize=4.5,
                    capsize=2, elinewidth=.8, zorder=4)
        stats=data['regimes'][regime]
        ax.set_title(f'{regime}  {label}\nMAE {stats["mae_lps"]:.3f} L/s; '
                     f'within band {stats["measured_band_pass"]}/{len(rr)}',
                     loc='left', fontsize=9, pad=9)
        ax.set(xlim=(0, limit), ylim=(0, limit), xlabel='Experimental |Q| (L/s)',
               ylabel='Simulated |Q| (L/s)', aspect='equal')
    fig.legend(handles=[Line2D([], [], color=BLUE, marker='o', ls='none', label='Physical-time smoothing, fine step'),
        Line2D([], [], color=GRAY, marker='o', mfc='none', ls='none', label='Archived per-step smoothing, coarse step'),
        Line2D([], [], color=GREEN, ls='--', label='1:1 agreement; horizontal bars = measured ξf')],
        loc='lower center', bbox_to_anchor=(.5, .025), frameon=False, fontsize=9)
    return fig


def convergence(data):
    fig, axes = canvas('Timestep sensitivity across every measured case',
        'Relative change = |Qcoarse − Qfine| / |Qfine| × 100 • fixed θ = 0.8 and reference interval 1 s\n'
        '300 s dry-start runs; exchange means over 240–300 s; mesh and physical coefficients held fixed')
    for ax, (regime, label) in zip(axes.flat, REGIMES):
        rr = [r for r in data['rows'] if r['regime'] == regime]
        x=np.arange(1, len(rr)+1)
        ax.axhspan(.001, 1, color='#e1efe6')
        ax.axhline(1, color=GREEN, ls='--', lw=1.1)
        for index, color, marker in [(0, ORANGE, '^'), (1, BLUE, 'o')]:
            ax.plot(x, [max(r['changes_pct'][index], .001) for r in rr],
                    color=color, marker=marker, markersize=4, lw=1)
        high=max(2, max(v for r in rr for v in r['changes_pct'])*1.5)
        ax.set(yscale='log', ylim=(.001, high), xlim=(.5, len(rr)+.5),
               xlabel=f'Table 2 row within {regime}', ylabel='Change in mean exchange (%)')
        ax.set_xticks(x if len(x)<10 else [1, 3, 5, 7, 9, 11, 13, 15])
        ax.set_title(f'{regime}  {label}\nMaximum final change '
                     f'{data["regimes"][regime]["max_fine_change_pct"]:.3f}%',
                     loc='left', fontsize=10, pad=9)
    fig.legend(handles=[Line2D([], [], color=ORANGE, marker='^', label='0.0125 → 0.00625 s'),
        Line2D([], [], color=BLUE, marker='o', label='0.00625 → 0.003125 s'),
        Line2D([], [], color=GREEN, ls='--', label='1% temporal comparison criterion')],
        loc='lower center', bbox_to_anchor=(.5, .02), ncol=2, frameon=False, fontsize=9)
    return fig


def build(sink):
    data=json.loads(DATA.read_text())
    for suffix, draw in [('parity', parity), ('convergence', convergence)]:
        name='hydraulics_ch9_rubinato_momentum_'+suffix
        if name in sink.expected:
            sink.save(draw(data), name)
            svg=sink.svg_dir/(name+'.svg')
            svg.write_text('\n'.join(line.rstrip() for line in svg.read_text().splitlines())+'\n')
