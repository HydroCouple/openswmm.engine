from pathlib import Path
import json
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=Path(__file__).resolve().parent;rows=json.loads((p/'mesh_results.json').read_text());x=np.arange(3);colors=['#17699b','#d96a24'];names=['Triangles','Quadrilaterals','Mixed cells']
fig,axs=plt.subplots(1,2,figsize=(12,4.7));fig.suptitle('A small depth improvement hides severe shoreline velocity spikes',fontsize=16,fontweight='bold',y=.99)
for v,label,color,off in [('baseline','Current solver',colors[0],-.19),('selected','Rejected shoreline fit',colors[1],.19)]:
 data=[next(r for r in rows if r['case']=='radial'and r['nx']==32 and r['order']==2 and r['quad']==q and r['variant']==v)for q in range(3)]
 for ax,key,scale,fmt in [(axs[0],'relative_l1',100,'.1f'),(axs[1],'max_speed',1,'.2g')]:
  val=[r[key]*scale for r in data];bars=ax.bar(x+off,val,.36,label=label,color=color)
  for b,a in zip(bars,val):ax.annotate(format(a,fmt),(b.get_x()+b.get_width()/2,a),ha='center',va='bottom',xytext=(0,4),textcoords='offset points',fontsize=10)
axs[0].set_title('Depth error after three periods');axs[0].set_ylabel('Relative depth L1 error (%)');axs[0].set_ylim(0,27)
axs[1].set_title('Peak speed at the sampled times');axs[1].set_ylabel('Speed (m/s, logarithmic scale)');axs[1].set_yscale('log');axs[1].set_ylim(.2,110)
for ax in axs:
 ax.set_xticks(x,names);ax.spines[['top','right']].set_visible(False);ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True)
axs[0].legend(frameon=False,loc='upper left',bbox_to_anchor=(0,-.13),ncol=2)
fig.text(.99,.02,'Radial Thacker bowl • 32 divisions • 15% vertex perturbation • CPU, second order',ha='right',fontsize=9,color='#555555')
fig.tight_layout(rect=[0,.05,1,.94]);fig.savefig(p/'comparison.png',dpi=170);fig.savefig(p/'comparison.svg')
