from pathlib import Path
import subprocess,json,os,random,resource,statistics
p=Path(__file__).resolve().parent;rows=[];rng=random.Random(103010)
jobs=[('radial',64,2,1,1,3),('planar',64,2,1,1,3),('planar',64,2,0,1,3),('planar',128,2,1,1,3),('planar',64,2,1,4,3),('planar',64,1,1,1,3),('ritter',256,2,0,1,2)]
for case,n,order,quad,threads,duration in jobs:
 for repeat in range(-1,5):
  vs=['baseline','selected'];rng.shuffle(vs);pair=[]
  for v in vs:
   start=resource.getrusage(resource.RUSAGE_CHILDREN);r=subprocess.run([str(p/f'build_{v}/performance'),case,str(n),'1',str(order),str(threads),str(quad),str(duration)],capture_output=True,text=True,check=True,timeout=300);end=resource.getrusage(resource.RUSAGE_CHILDREN);row=json.loads(r.stdout);pair.append({k:x for k,x in row.items()if k!='seconds'})
   row.update(cpu_seconds=end.ru_utime-start.ru_utime+end.ru_stime-start.ru_stime,variant=v,repeat=repeat,load_average=os.getloadavg());rows.append(row);(p/'performance_results.json').write_text(json.dumps(rows,indent=2))
  assert pair[0]==pair[1];print(case,n,quad,order,threads,repeat,flush=True)
summary=[]
for case,n,order,quad,threads,duration in jobs:
 rr=[r for r in rows if (r['case'],r['nx'],r['order'],r['quad'],r['threads'])==(case,n,order,quad,threads)and r['repeat']>=0]
 ratios=[next(r['cpu_seconds']for r in rr if r['repeat']==i and r['variant']=='selected')/next(r['cpu_seconds']for r in rr if r['repeat']==i and r['variant']=='baseline')for i in range(5)]
 summary.append(dict(case=case,n=n,order=order,quad=quad,threads=threads,median_cpu_ratio=statistics.median(ratios),paired_ratios=ratios,baseline_cpu=statistics.median(r['cpu_seconds']for r in rr if r['variant']=='baseline'),selected_cpu=statistics.median(r['cpu_seconds']for r in rr if r['variant']=='selected')))
(p/'performance_summary.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2),flush=True)
