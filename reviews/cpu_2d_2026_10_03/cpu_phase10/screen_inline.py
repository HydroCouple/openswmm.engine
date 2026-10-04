from pathlib import Path
import subprocess,json,resource,random,statistics
p=Path(__file__).resolve().parent;rows=[];rng=random.Random(10010)
for case,q,order in [('planar',0,2),('planar',1,2),('planar',1,1),('radial',1,2)]:
 for repeat in range(-1,3):
  vs=['baseline','inlined'];rng.shuffle(vs)
  for v in vs:
   t=resource.getrusage(resource.RUSAGE_CHILDREN);r=subprocess.run([str(p/f'build_{v}/performance'),case,'64','1',str(order),'1',str(q),'3'],capture_output=True,text=True,check=True);u=resource.getrusage(resource.RUSAGE_CHILDREN);row=json.loads(r.stdout);row.update(variant=v,repeat=repeat,cpu=u.ru_utime-t.ru_utime+u.ru_stime-t.ru_stime);rows.append(row)
  print(case,q,order,repeat,flush=True);(p/'inline_screen.json').write_text(json.dumps(rows,indent=2))
 for v in ['baseline','inlined']:print(case,q,order,v,statistics.median(r['cpu']for r in rows if r['variant']==v and r['case']==case and r['quad']==q and r['order']==order and r['repeat']>=0),flush=True)
