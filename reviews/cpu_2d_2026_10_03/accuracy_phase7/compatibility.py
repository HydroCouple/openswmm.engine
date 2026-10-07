from pathlib import Path
import subprocess,json,os,hashlib,csv
p=Path(__file__).resolve().parent;rows=[]
for case in ['lake_wet','lake_dry','ritter','stoker','radial','planar']:
 for quad in [0,1]:
  for tiers,order in [(1,1),(4,1),(1,2)]:
   duration='1'if case in ['radial','planar']else '2'if case in ['ritter','stoker']else '10';ref=None
   for v in ['baseline','selected']:
    f=p/'fields.tmp';h=p/'history.tmp';r=subprocess.run([str(p/f'build_{v}/review'),case,'32',str(tiers),str(order),'4',str(quad),duration],env={**os.environ,'ANALYTICAL_FIELDS':str(f),'REVIEW_HISTORY':str(h)},capture_output=True,text=True,check=True,timeout=180);row=json.loads(r.stdout);sha=hashlib.sha256(f.read_bytes()+h.read_bytes()).hexdigest();f.unlink();h.unlink()
    if ref is None:ref=sha
    must_equal=order==1 or case in ['ritter','stoker']
    assert not must_equal or ref==sha,(case,quad,tiers,order,v)
    if case in ['lake_wet','lake_dry']:assert row['max_depth_change']<1e-12,row
    assert row['min_depth']>=0 and abs(row['volume_relative_error'])<1e-10,row
    row.update(variant=v,field_and_history_hash=sha,expected_equal=must_equal);rows.append(row)
   print(case,quad,tiers,order,'pass',flush=True)
   (p/'compatibility_results.json').write_text(json.dumps(rows,indent=2))
print('PASS',len(rows),'compatibility runs',flush=True)
# Selected second-order trajectories must remain reproducible across threads.
threadrows=[]
for case in ['radial','planar']:
 for quad in [0,1]:
  ref=None
  for threads in [1,4]:
   f=p/'fields.tmp';h=p/'history.tmp';r=subprocess.run([str(p/'build_selected/review'),case,'64','1','2',str(threads),str(quad),'3'],env={**os.environ,'ANALYTICAL_FIELDS':str(f),'REVIEW_HISTORY':str(h)},capture_output=True,text=True,check=True,timeout=180);sha=hashlib.sha256(f.read_bytes()+h.read_bytes()).hexdigest();f.unlink();h.unlink()
   if ref is None:ref=sha
   assert sha==ref,(case,quad,threads)
   row=json.loads(r.stdout);row['field_and_history_hash']=sha;threadrows.append(row)
(p/'thread_results.json').write_text(json.dumps(threadrows,indent=2));print('PASS 8 cross-thread trajectories',flush=True)
