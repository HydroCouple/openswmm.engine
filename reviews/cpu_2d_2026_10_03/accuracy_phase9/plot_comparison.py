from pathlib import Path
import json,csv,math
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=Path(__file__).resolve().parent;screen=json.loads((p/'screen.json').read_text());cand=json.loads((p/'stencil_results.json').read_text());perf=json.loads((p/'performance_summary.json').read_text());energy=json.loads((p/'energy_results.json').read_text());labels=['Radial\ntriangles','Radial\nquads','Planar\ntriangles','Planar\nquads'];cases=[(c,q)for c in ['radial','planar']for q in [0,1]];x=np.arange(4)
fig,axs=plt.subplots(2,2,figsize=(13,8.6));fig.suptitle('Shoreline correction: lower error and less CPU for comparable accuracy',fontsize=16,fontweight='bold');colors=['#236a99','#15967d']
for v,label,color,off in [('baseline','Before',colors[0],-.18),('selected','After',colors[1],.18)]:
 rs=screen if v=='baseline'else cand;variant='baseline'if v=='baseline'else 'connected_first_velocity';errors=[100*next(r['relative_l1']for r in rs if r['variant']==variant and r['case']==c and r['quad']==q and r['nx']==64 and r['skew']==0)for c,q in cases];bars=axs[0,0].bar(x+off,errors,.34,color=color,label=label);axs[0,0].bar_label(bars,fmt='%.1f',padding=3,fontsize=9)
 vals=[100*next(r['oscillatory_energy_ratio']for r in energy if r['variant']==v and r['case']==c and r['quad']==q)for c,q in cases];bars=axs[1,0].bar(x+off,vals,.34,color=color);axs[1,0].bar_label(bars,fmt='%.0f',padding=3,fontsize=9)
 a=[next(r for r in perf if r['comparison']=='accuracy_cost'and r['case']==c and r['quad']==q)for c,q in cases];vals=[r[v]['cpu_seconds']for r in a];bars=axs[0,1].bar(x+off,vals,.34,color=color,label='Before: 128 divisions'if v=='baseline'else 'After: 96 divisions');axs[0,1].bar_label(bars,fmt='%.2f',padding=3,fontsize=9)
axs[0,0].set(title='Depth error • same 64-division mesh',ylabel='Relative depth L1 error (%)',ylim=(0,34));axs[0,0].legend(frameon=False)
axs[1,0].set(title='Energy above equilibrium • same mesh',ylabel='Remaining after three periods (%)',ylim=(0,100))
axs[0,1].set(title='CPU cost • after uses fewer cells and has lower error',ylabel='Median child CPU time (s)');axs[0,1].legend(frameon=False,fontsize=9)
for v,label,color in [('baseline','Before',colors[0]),('selected','After',colors[1])]:
 hist=p/'histories'/('planar_64_1_0_baseline.csv'if v=='baseline'else 'mesh_planar_64_1_2_1_0_selected.csv');h=list(csv.DictReader(hist.open()));t=np.array([float(r['t'])for r in h]);phase=np.unwrap(np.arctan2([float(r['cy'])for r in h],[float(r['cx'])for r in h]));period=2*math.pi/math.sqrt(2*9.80665*.1);axs[1,1].plot(t/period,(phase-2*math.pi*t/period)*180/math.pi,label=label,color=color,lw=2)
axs[1,1].set(title='Planar quad bowl • phase error',xlabel='Oscillation periods',ylabel='Centroid phase error (degrees)');axs[1,1].axhline(0,color='#777',lw=.8,ls='--');axs[1,1].legend(frameon=False)
for ax in axs.flat:
 ax.spines[['top','right']].set_visible(False);ax.grid(axis='y',alpha=.18);ax.set_axisbelow(True)
for ax in [axs[0,0],axs[0,1],axs[1,0]]:ax.set_xticks(x,labels,fontsize=9)
fig.text(.5,.013,'CPU only • three-period analytical bowls • same-mesh CPU cost rises about 10–17% • timing details and limits in REPORT.md',ha='center',fontsize=9,color='#555')
fig.tight_layout(rect=[0,.035,1,.955]);fig.savefig(p/'comparison.png',dpi=170);fig.savefig(p/'comparison.svg')
