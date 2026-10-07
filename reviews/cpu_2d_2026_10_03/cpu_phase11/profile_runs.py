from pathlib import Path
import subprocess,json
p=Path(__file__).resolve().parent;rows=[]
for case,q in [('planar',0),('planar',1),('radial',1)]:
 r=subprocess.run([str(p/'build_profile/review'),case,'64','1','2','1',str(q),'3'],capture_output=True,text=True,check=True)
 timers={x.split()[1]:dict(seconds=float(x.split()[2]),calls=int(x.split()[3]))for x in r.stderr.splitlines()if x.startswith('PROFILE ')}
 rows.append(dict(case=case,quad=q,timers=timers));print(case,q,timers,flush=True)
(p/'profiles.json').write_text(json.dumps(rows,indent=2))
