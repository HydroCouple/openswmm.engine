from pathlib import Path
import subprocess,json,os,random,resource,statistics
p=Path(__file__).resolve().parent;profiles=[]
for case,q in [('planar',0),('planar',1),('radial',1)]:
 args=[case,'64','1','2','1',str(q),'3']
 r=subprocess.run([str(p/'build_profile/review'),*args],capture_output=True,text=True,check=True)
 timers={x.split()[1]:dict(seconds=float(x.split()[2]),calls=int(x.split()[3]))for x in r.stderr.splitlines()if x.startswith('PROFILE ')}
 profiles.append(dict(case=case,quad=q,timers=timers));print('PROFILE',case,q,timers,flush=True)
(p/'profiles.json').write_text(json.dumps(profiles,indent=2))
rows=[];rng=random.Random(1010)
for case,q,threads in [('planar',0,1),('planar',1,1),('radial',1,1),('planar',1,4)]:
 for repeat in range(-1,3):
  vs=['baseline','direct','cached'];rng.shuffle(vs)
  for v in vs:
   t=resource.getrusage(resource.RUSAGE_CHILDREN);r=subprocess.run([str(p/f'build_{v}/performance'),case,'64','1','2',str(threads),str(q),'3'],capture_output=True,text=True,check=True);u=resource.getrusage(resource.RUSAGE_CHILDREN);row=json.loads(r.stdout)
   row.update(variant=v,repeat=repeat,cpu=u.ru_utime-t.ru_utime+u.ru_stime-t.ru_stime,load=os.getloadavg());rows.append(row)
  print(case,q,threads,repeat,flush=True);(p/'screen.json').write_text(json.dumps(rows,indent=2))
for case,q,threads in [('planar',0,1),('planar',1,1),('radial',1,1),('planar',1,4)]:
 group=[r for r in rows if r['case']==case and r['quad']==q and r['threads']==threads and r['repeat']>=0]
 med={v:statistics.median(r['cpu']for r in group if r['variant']==v)for v in ['baseline','direct','cached']};print(case,q,threads,med,flush=True)
