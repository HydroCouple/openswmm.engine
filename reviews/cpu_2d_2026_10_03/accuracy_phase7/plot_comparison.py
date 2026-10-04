from pathlib import Path
import os
os.environ.setdefault('MPLCONFIGDIR','/tmp/openswmm-review-matplotlib')
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=Path(__file__).resolve().parent;fig,axes=plt.subplots(2,3,figsize=(14,8),layout='constrained');colors={'baseline':'#a85624','selected':'#007e87'};period=2*np.pi/np.sqrt(2*9.80665*.1)
for row,quad in enumerate([0,1]):
 shape='Triangles'if quad==0 else 'Quadrilaterals'
 for v in ['baseline','selected']:
  h=np.genfromtxt(p/'histories'/f'planar_64_{quad}_3_{v}.csv',delimiter=',',names=True);label='Before'if v=='baseline'else 'Phase 7';color=colors[v]
  axes[row,0].plot(h['cx'],h['cy'],color=color,label=label,lw=1.5);axes[row,0].scatter(h['cx'][-1],h['cy'][-1],color=color,s=35)
  axes[row,1].plot(h['t']/period,100*h['relative_l1'],color=color,label=label,lw=1.7)
  axes[row,2].plot(h['t']/period,100*h['energy']/h['energy'][0],color=color,label=label,lw=1.7)
 theta=np.linspace(0,2*np.pi,300);axes[row,0].plot(.5*np.cos(theta),.5*np.sin(theta),'--',color='#59636e',label='Exact',lw=1);axes[row,0].scatter([.5],[0],marker='x',color='#222222',s=50)
 axes[row,0].set(xlabel='Centroid x (m)',ylabel='Centroid y (m)',title=shape+' · motion',aspect='equal',xlim=(-.56,.56),ylim=(-.56,.56));axes[row,1].set(xlabel='Periods',ylabel='Relative depth L1 (%)',title=shape+' · depth error',ylim=(0,55));axes[row,2].axhline(100,color='#59636e',ls='--',lw=1,label='Conserved target');axes[row,2].set(xlabel='Periods',ylabel='Energy / initial energy (%)',title=shape+' · energy retention',ylim=(75,102))
 for ax in axes[row]:ax.grid(alpha=.18);ax.spines[['top','right']].set_visible(False);ax.legend(fontsize=9,loc='best')
fig.suptitle('Planar Thacker bowl · 64 × 64 divisions · second order · three periods\nPhase improves; numerical damping remains',fontsize=16)
fig.savefig(p/'comparison.png',dpi=160);fig.savefig(p/'comparison.svg');plt.close(fig)
