from pathlib import Path
import subprocess,json,csv,os,math
p=Path(__file__).resolve().parent;rows=[]
for case,phase in [('radial',0),('planar',.7),('planar',2.1)]:
 for quad in [0,1,2]:
  for v in ['baseline','selected']:
   hist=p/'histories'/f'stress_{case}_{phase}_{quad}_{v}.csv';env={**os.environ,'REVIEW_HISTORY':str(hist),'REVIEW_SKEW':'.3','REVIEW_PHASE':str(phase),'REVIEW_DISPLACEMENT':'.8','REVIEW_RADIAL_AMPLITUDE':'.45'}
   r=subprocess.run([str(p/f'build_{v}/stress'),case,'32','1','2','1',str(quad),'3'],capture_output=True,text=True,env=env,check=True,timeout=180);row=json.loads(r.stdout);h=list(csv.DictReader(hist.open()));row.update(variant=v,phase=phase,skew=.3,max_speed=max(float(x['max_speed'])for x in h),mean_relative_l1=sum(float(x['relative_l1'])for x in h)/len(h));rows.append(row);(p/'stress_results.json').write_text(json.dumps(rows,indent=2));print(case,phase,quad,v,'L1',row['relative_l1'],'maxV',row['max_speed'],flush=True)
   assert row['min_depth']>=0 and abs(row['volume_relative_error'])<1e-11
