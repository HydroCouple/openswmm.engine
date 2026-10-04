from pathlib import Path
import subprocess,json,os,csv,math
p=Path(__file__).resolve().parent;rows=[]
for case in ['radial','planar']:
 for quad in [0,1]:
  for tag,extra in [('default',{}),('small_cfl',{'REVIEW_CFL':'.1'}),('beta_one',{'REVIEW_BETA':'1'})]:
   history=p/'histories'/f'diagnostic_{case}_{quad}_{tag}.csv';r=subprocess.run([str(p/'build_instrumented/review'),case,'64','1','2','1',str(quad),'3'],capture_output=True,text=True,env={**os.environ,**extra,'REVIEW_HISTORY':str(history)},check=True,timeout=150)
   row=json.loads(r.stdout);row.update(tag=tag,counters=json.loads(r.stderr.split('REVIEW ')[-1]));h=list(csv.DictReader(history.open()));row['max_speed']=max(float(x['max_speed'])for x in h);rows.append(row)
   c=row['counters'];print(case,quad,tag,'L1',row['relative_l1'],'fallback_volume',sum(c['volume'][1:4])/c['volume'][0],'blocked_volume',c['volume'][3]/c['volume'][0],'capped_fraction',c['capped']/c['faces'],'removed_export_fraction',c['removed_export']/c['raw_export'],flush=True)
   (p/'diagnosis.json').write_text(json.dumps(rows,indent=2))
