from pathlib import Path
import subprocess,json,random,resource,statistics,os
p=Path(__file__).resolve().parent;rows=[];rng=random.Random(11035)
for case,q,threads in [('planar',0,1),('planar',1,1),('planar',1,4)]:
 for repeat in range(-1,5):
  vs=['baseline','extrema'];rng.shuffle(vs);states=[]
  for v in vs:
   t=resource.getrusage(resource.RUSAGE_CHILDREN);r=subprocess.run([str(p/f'build_{v}/performance'),case,'64','1','2',str(threads),str(q),'1'],capture_output=True,text=True,check=True);u=resource.getrusage(resource.RUSAGE_CHILDREN);row=json.loads(r.stdout);states.append({k:x for k,x in row.items()if k!='seconds'});row.update(variant=v,repeat=repeat,cpu=u.ru_utime-t.ru_utime+u.ru_stime-t.ru_stime,load=os.getloadavg());rows.append(row)
  assert states[0]==states[1];(p/'confirmation.json').write_text(json.dumps(rows,indent=2));print(q,threads,repeat,flush=True)
