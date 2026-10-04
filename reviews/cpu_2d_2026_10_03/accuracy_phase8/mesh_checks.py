from pathlib import Path
import subprocess,json,csv,os,math,hashlib
p=Path(__file__).resolve().parent;rows=[];jobs=[]
# Distorted triangles, quads, and mixed cells, with first-order compatibility.
for quad in [0,1,2]:
 for case in ['radial','planar','lake_wet','lake_dry','ritter','stoker']:
  for order in [1,2]:jobs.append((case,32,quad,order,1,.15))
# Cross-thread comparison on regular and mixed/distorted shoreline flows.
for quad,skew in [(0,0),(1,0),(2,.15)]:
 for case in ['radial','planar']:
  for threads in [1,4]:jobs.append((case,64,quad,2,threads,skew))
for case,n,quad,order,threads,skew in jobs:
 variants=['selected']if n==64 else ['baseline','selected']
 for v in variants:
  name=f'mesh_{case}_{n}_{quad}_{order}_{threads}_{skew}_{v}';hist=p/'histories'/f'{name}.csv';fields=p/'fields'/f'{name}.csv';fields.parent.mkdir(exist_ok=True)
  duration=3 if case in ['radial','planar']else 20 if case.startswith('lake')else 2
  r=subprocess.run([str(p/f'build_{v}/review'),case,str(n),'1',str(order),str(threads),str(quad),str(duration)],capture_output=True,text=True,env={**os.environ,'REVIEW_SKEW':str(skew),'REVIEW_HISTORY':str(hist),'ANALYTICAL_FIELDS':str(fields)},check=True,timeout=180)
  row=json.loads(r.stdout);h=list(csv.DictReader(hist.open()));row.update(variant=v,skew=skew,max_speed=max(float(x['max_speed'])for x in h),history_sha=hashlib.sha256(hist.read_bytes()).hexdigest(),fields_sha=hashlib.sha256(fields.read_bytes()).hexdigest());rows.append(row)
  assert row['min_depth']>=0 and abs(row['volume_relative_error'])<1e-11,row
  if case.startswith('lake'):assert row['max_depth_change']<1e-12 and row['max_speed']<1e-9,row
  print(case,n,quad,order,threads,skew,v,'L1',round(row['relative_l1'],5),'maxV',round(row['max_speed'],3),flush=True)
  (p/'mesh_results.json').write_text(json.dumps(rows,indent=2))
for r in rows:
 if r['nx']==32 and r['order']==1 and r['variant']=='baseline':
  t=next(x for x in rows if all(x[k]==r[k]for k in ['case','quad','order','threads','skew'])and x['variant']=='selected'and x['nx']==32);assert r['fields_sha']==t['fields_sha'] and r['history_sha']==t['history_sha']
 if r['nx']==64 and r['threads']==1:
  t=next(x for x in rows if all(x[k]==r[k]for k in ['case','quad','order','skew'])and x['threads']==4 and x['nx']==64);assert r['fields_sha']==t['fields_sha'] and r['history_sha']==t['history_sha']
print('PASS',len(rows),'runs; conservation, positivity, resting lakes; exact first-order and thread comparisons',flush=True)
