from pathlib import Path
import json,math
p=Path(__file__).resolve().parent;groups={}
for line in (p/'budget_trace.log').read_text().splitlines():
 if not line.startswith('FACE '):continue
 a=line.split();row=dict(zip(['pass','face','neighbor','dt','h','qx','qy','dv','flux_x','flux_y','source_x','source_y','gradient_x','gradient_y','area'],map(float,a[1:])));groups.setdefault(int(row['pass']),[]).append(row)
out=[]
for k,rows in groups.items():
 r=rows[0];dv=sum(x['dv']for x in rows);dx=sum(x['flux_x']+x['source_x']for x in rows);dy=sum(x['flux_y']+x['source_y']for x in rows);h=r['h'];hn=h+dv/r['area'];qx=r['qx']+dx/r['area'];qy=r['qy']+dy/r['area'];before=math.hypot(r['qx'],r['qy'])/h if h>1e-7 else 0;after=math.hypot(qx,qy)/hn if hn>1e-7 else 0
 if after>5 or(before>1 and after>before*1.5):out.append(dict(pass_id=k,before_depth=h,after_depth=hn,before_speed=before,after_speed=after,relative_water_change=dv/(h*r['area'])if h else 0,eta_gradient=math.hypot(r['gradient_x'],r['gradient_y']),faces=rows))
(p/'budget_events.json').write_text(json.dumps(out,indent=2))
for r in out[:6]:print({k:v for k,v in r.items()if k!='faces'})
