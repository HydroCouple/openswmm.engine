from pathlib import Path
import subprocess,json,csv,os,math
p=Path(__file__).resolve().parent;rows=[]
for case in ['radial','planar']:
 for quad in [0,1]:
  hist=p/'histories'/f'cost_{case}_{quad}_selected.csv';r=subprocess.run([str(p/'build_selected/review'),case,'96','1','2','1',str(quad),'3'],capture_output=True,text=True,env={**os.environ,'REVIEW_HISTORY':str(hist)},check=True,timeout=180);row=json.loads(r.stdout);h=list(csv.DictReader(hist.open()));row.update(variant='selected',max_speed=max(float(x['max_speed'])for x in h),mean_relative_l1=sum(float(x['relative_l1'])for x in h)/len(h));rows.append(row);print(case,quad,'L1',row['relative_l1'],'maxV',row['max_speed'],flush=True);(p/'accuracy_cost_results.json').write_text(json.dumps(rows,indent=2))
