from pathlib import Path
import subprocess,json,random,resource,statistics,os
p=Path(__file__).resolve().parent;rows=[];rng=random.Random(11031)
jobs=[('planar',0,2,1),('planar',1,2,1),('radial',1,2,1),('planar',1,2,4)]
variants=['baseline','limiter','friction','clear','combined','extrema']
for case,q,order,threads in jobs:
 for repeat in range(-1,3):
  vs=variants.copy();rng.shuffle(vs);pair=[]
  for v in vs:
   t=resource.getrusage(resource.RUSAGE_CHILDREN);r=subprocess.run([str(p/f'build_{v}/performance'),case,'32','1',str(order),str(threads),str(q),'3'],capture_output=True,text=True,check=True);u=resource.getrusage(resource.RUSAGE_CHILDREN);row=json.loads(r.stdout);pair.append({k:x for k,x in row.items()if k!='seconds'});row.update(variant=v,repeat=repeat,cpu=u.ru_utime-t.ru_utime+u.ru_stime-t.ru_stime,load=os.getloadavg());rows.append(row)
  assert all(x==pair[0]for x in pair);(p/'screen.json').write_text(json.dumps(rows,indent=2));print(case,q,threads,repeat,flush=True)
 for v in variants:
  rr=[r for r in rows if r['variant']==v and r['case']==case and r['quad']==q and r['order']==order and r['threads']==threads and r['repeat']>=0]
  print(case,q,threads,v,statistics.median(r['cpu']for r in rr),flush=True)
