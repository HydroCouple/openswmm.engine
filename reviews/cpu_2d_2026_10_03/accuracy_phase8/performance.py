from pathlib import Path
import subprocess,json,os,random,resource
p=Path(__file__).resolve().parent;rows=[];rng=random.Random(73473)
jobs=[('radial',64,2,1,3),('planar',64,2,1,3),('planar',64,2,0,3),('planar',64,1,1,3),('ritter',256,2,0,2)]
for case,n,order,quad,duration in jobs:
 for repeat in range(-1,5):
  vs=['baseline','selected'];rng.shuffle(vs)
  for v in vs:
   start=resource.getrusage(resource.RUSAGE_CHILDREN);r=subprocess.run([str(p/f'build_{v}/performance'),case,str(n),'1',str(order),'1',str(quad),str(duration)],capture_output=True,text=True,check=True,timeout=180);end=resource.getrusage(resource.RUSAGE_CHILDREN);row=json.loads(r.stdout)
   # Child CPU includes initialization, while seconds is advancement only.
   row.update(cpu_seconds=(end.ru_utime-start.ru_utime)+(end.ru_stime-start.ru_stime),variant=v,repeat=repeat,load_average=os.getloadavg())
   if repeat>=0:rows.append(row);(p/'performance_results.json').write_text(json.dumps(rows,indent=2))
  print(case,n,order,quad,repeat,flush=True)
print('PASS',len(rows),'timed runs',flush=True)
