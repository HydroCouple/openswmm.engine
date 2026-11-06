"""CPU review diagrams and measured accuracy/cost chart; no engine execution.

The two diagrams are schematic. The chart uses the committed phase-12
measurements copied into data/2d_cpu_review.json with source provenance.
"""
from pathlib import Path
import json
import numpy as np
REQUIRES = ()

def reconstruction():
    import matplotlib.pyplot as plt
    from matplotlib.patches import Polygon
    fig,axs=plt.subplots(1,2,figsize=(9.4,3.7))
    ax=axs[0];x=np.linspace(0,1,100);z=.15+.4*x;eta=.95+.12*x
    ax.fill_between(x,z,eta,color='#dcecf7');ax.plot(x,z,color='#8b643f',lw=2,label='face bed: z = η − h');ax.plot(x,eta,color='#1877b9',lw=2,label='reconstructed surface η')
    for xx,lab in [(.18,'cell centre'),(.82,'face midpoint')]:
        zz=.15+.4*xx;ee=.95+.12*xx
        ax.annotate('',(xx,ee),(xx,zz),arrowprops=dict(arrowstyle='<->',color='#29394b'));ax.text(xx+.035,(ee+zz)/2,'h',fontsize=10);ax.text(xx,.04,lab,ha='center',fontsize=8)
    ax.text(.5,1.19,'Reconstruct η and h; derive z at the face',ha='center',fontsize=9)
    ax.set(xlim=(-.06,1.06),ylim=(0,1.3));ax.axis('off');ax.legend(loc='lower left',bbox_to_anchor=(0,-.17),frameon=False,fontsize=8)
    ax.set_title('(a) Consistent face geometry',loc='left',fontsize=11)
    ax=axs[1]
    for xy,color,label in [((0,0),'#dcecf7','target'),((-1,0),'#83badf','wet'),((0,1),'#83badf','wet'),((1,0),'#e8e4de','dry / blocked')]:
        xx,yy=xy;ax.add_patch(Polygon([(xx-.45,yy-.45),(xx+.45,yy-.45),(xx+.45,yy+.45),(xx-.45,yy+.45)],fc=color,ec='white',lw=2));ax.text(xx,yy,label,ha='center',va='center',fontsize=8)
    for end in [(-1,0),(0,1)]:ax.annotate('',end,(0,0),arrowprops=dict(arrowstyle='->',color='#1877b9',shrinkA=16,shrinkB=16))
    ax.text(0,-.75,'Fit η, h from connected wet neighbours\nKeep the target cell’s velocity',ha='center',va='top',fontsize=9,linespacing=1.4)
    ax.text(0,-1.4,'Thin / insufficient / ill-conditioned stencil → first order\nLimit depth at every face midpoint',ha='center',va='top',fontsize=8,linespacing=1.4)
    ax.set(xlim=(-1.65,1.65),ylim=(-1.9,1.65));ax.axis('off');ax.set_title('(b) Shoreline reconstruction',loc='left',fontsize=11)
    fig.tight_layout(pad=.8,w_pad=2);return fig

def pipeline():
    import matplotlib.pyplot as plt
    from matplotlib.patches import FancyBboxPatch
    fig,ax=plt.subplots(figsize=(9.4,3.9));ax.set(xlim=(0,10),ylim=(0,4.3));ax.axis('off')
    def box(x,y,w,h,t,color='#e7eff5'):
        ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle='round,pad=.04',fc=color,ec='#56738a'));ax.text(x+w/2,y+h/2,t,ha='center',va='center',fontsize=9,linespacing=1.45)
    def arrow(a,b):ax.annotate('',b,a,arrowprops=dict(arrowstyle='->',color='#324e64',lw=1.3))
    box(.1,2.55,2.4,1.1,'Unique face transfer\n[s₀ s₁ … sₙ] per face\nSeparate left / right buffers')
    box(3,2.55,2.8,1.1,'Cell-owned incidence gather\nUp to 8 species per block\nSame edge order for each species')
    arrow((2.55,3.1),(2.96,3.1))
    box(6.45,3,3.3,.8,'≥ 8 species\nGather row → apply its sources','#d9ecdf');box(6.45,1.75,3.3,.8,'< 8 species\nGather pass → separate source pass')
    arrow((5.85,3.15),(6.4,3.4));arrow((5.85,2.8),(6.4,2.2))
    box(.7,.65,4.6,.95,'One cell water budget after face volume lands\nApplied sinks and gross inflows retained','#fff0d1')
    arrow((5.35,1.12),(6.45,1.95));arrow((5.35,1.35),(6.45,3.1))
    ax.text(5,.1,'Worker-private ledgers → fold after the parallel region • Public cell-mass layout unchanged',ha='center',fontsize=9)
    ax.set_title('CPU transport locality and conditional fusion',loc='left',fontsize=12,pad=8)
    fig.tight_layout(pad=.65);return fig

def accuracy():
    import matplotlib.pyplot as plt
    data=json.loads((Path(__file__).resolve().parents[2]/'data/2d_cpu_review.json').read_text());s=data['timings'];checks=data['accuracy'];labels=['Radial\ntriangles','Radial\nquads','Planar\ntriangles','Planar\nquads'];x=np.arange(4)
    fig,axs=plt.subplots(1,2,figsize=(9.4,3.8))
    ratios=[r['median_cpu_ratio'] for r in s];ax=axs[0];ax.bar(x,ratios,color='#317eb0',width=.6)
    for i,r in enumerate(s):ax.scatter([i-.12,i,i+.12],r['paired_cpu_ratios'],s=15,color='#162f43',zorder=3)
    ax.axhline(1,color='#777777',ls='--',lw=1);ax.text(3.45,1.025,'reference = 1',ha='right',fontsize=8);ax.set_ylim(0,1.13);ax.set_ylabel('CPU time / reference');ax.set_title('(a) CPU cost: median and measured pairs',fontsize=10,loc='left')
    ax=axs[1]
    for v,offset,color,label in [('baseline',-.18,'#a7b8c5','Earlier, 128 divisions'),('selected',.18,'#317eb0','Final, 96 divisions')]:
        vals=[100*next(a['relative_l1'] for a in checks if (a['case'],a['quad'],a['variant'])==(r['case'],r['quad'],v))for r in s];ax.bar(x+offset,vals,width=.34,color=color,label=label)
    ax.set_ylabel('Endpoint relative depth L1 (%)');ax.set_title('(b) Smaller final mesh, lower depth error',fontsize=10,loc='left');ax.legend(fontsize=8,frameon=False)
    for ax in axs:ax.set_xticks(x,labels,fontsize=8);ax.spines[['top','right']].set_visible(False);ax.grid(axis='y',alpha=.18);ax.set_axisbelow(True)
    fig.text(.5,.02,'Three Thacker periods • One CPU thread • Phase-7 reference vs final • Different resolutions',ha='center',fontsize=8)
    fig.tight_layout(rect=(0,.06,1,1),pad=.8,w_pad=2);return fig

def build(sink):
    for name,draw in [('hydraulics_ch9_reconstruction',reconstruction),('eng_2d_cpu_pipeline',pipeline),('hydraulics_ch9_accuracy_cost',accuracy)]:
        if name in sink.expected:
            sink.save(draw(),name)
            svg = sink.svg_dir / (name + ".svg")
            svg.write_text("\n".join(line.rstrip() for line in svg.read_text().splitlines()) + "\n")
