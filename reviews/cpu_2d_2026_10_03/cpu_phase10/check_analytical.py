from pathlib import Path
import subprocess,json,os,hashlib,math
p=Path(__file__).resolve().parent;(p/'histories').mkdir(exist_ok=True);(p/'fields').mkdir(exist_ok=True)
jobs=[]
for case in ['radial','planar']:
 for n in [32,64]:
  for quad in [0,1,2]:
   for skew in [0,.15]:jobs.append((case,n,quad,skew,2,1,3))
 for quad in [0,1]:jobs.append((case,64,quad,0,2,1,10))
 for quad in [0,1,2]:
  for skew in [0,.15]:jobs.append((case,32,quad,skew,1,1,3))
  jobs.append((case,32,quad,.15,2,4,3))
for case in ['lake_wet','lake_dry','ritter','stoker']:
 for quad in [0,1,2]:
  for order in [1,2]:jobs.append((case,128,quad,0,order,1,2))
rows=[]
for idx,(case,n,quad,skew,order,threads,duration) in enumerate(jobs):
 pair=[]
 for v in ['baseline','selected']:
  hist=p/'histories'/f'{idx}_{v}.csv';field=p/'fields'/f'{idx}_{v}.csv'
  env={**os.environ,'REVIEW_SKEW':str(skew),'REVIEW_HISTORY':str(hist),'ANALYTICAL_FIELDS':str(field)}
  r=subprocess.run([str(p/f'build_{v}/review'),case,str(n),'1',str(order),str(threads),str(quad),str(duration)],env=env,capture_output=True,text=True,check=True,timeout=240)
  row=json.loads(r.stdout);assert all(math.isfinite(x)for x in row.values()if isinstance(x,(float,int)));assert row['min_depth']>=0;assert abs(row['volume_relative_error'])<1e-11
  row.pop('seconds');row.update(history_sha256=hashlib.sha256(hist.read_bytes()).hexdigest(),fields_sha256=hashlib.sha256(field.read_bytes()).hexdigest());pair.append(row)
 assert pair[0]==pair[1],(idx,case,n,quad,skew,order,threads,pair)
 if threads==4:
  one=next(x for x in rows if all(x[k]==pair[0][k]for k in ['case','nx','quad','order','duration'])and x['skew']==skew and x['threads']==1)
  assert one['history_sha256']==pair[0]['history_sha256']and one['fields_sha256']==pair[0]['fields_sha256']
 rows.append(dict(**pair[0],skew=skew,baseline_selected_exact=True));(p/'analytical_results.json').write_text(json.dumps(rows,indent=2));print(idx+1,len(jobs),case,n,quad,skew,order,threads,'exact',flush=True)
