"""Run a small reverse-outfall deck against an explicitly selected engine."""
import ctypes as c
import json
import sys
from pathlib import Path
lib = c.CDLL(str(Path(sys.argv[1]).resolve()))
out = Path(sys.argv[2]).resolve(); out.mkdir(parents=True, exist_ok=True)
H,D,I=c.c_void_p,c.c_double,c.c_int

def bind(name,args,ret=I):
 f=getattr(lib,name);f.argtypes=args;f.restype=ret;return f
create=bind('swmm_engine_create',[],H)
open_=bind('swmm_engine_open',[H,c.c_char_p,c.c_char_p,c.c_char_p,c.c_char_p])
init=bind('swmm_engine_initialize',[H]);start=bind('swmm_engine_start',[H,I])
step=bind('swmm_engine_step',[H,c.POINTER(D)]);end=bind('swmm_engine_end',[H])
report=bind('swmm_engine_report',[H]);close=bind('swmm_engine_close',[H]);destroy=bind('swmm_engine_destroy',[H],None)
mb=bind('swmm_2d_get_mass_balance',[H]+[c.POINTER(D)]*10)
err=bind('swmm_2d_get_continuity_error',[H,c.POINTER(D)])
err1=bind('swmm_get_routing_continuity_error',[H,c.POINTER(D)])
results=[]
for routing in ['DYNWAVE','FV']:
 for sync in [0,6]:
  tag=f'{routing}_{sync}';inp=out/(tag+'.inp');rpt=out/(tag+'.rpt')
  inp.write_text(f'''[OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING {routing}
START_DATE 01/01/2026
START_TIME 00:00:00
END_DATE 01/01/2026
END_TIME 00:01:00
REPORT_STEP 00:00:10
ROUTING_STEP 2
VARIABLE_STEP 0
THREADS 1
[STORAGE]
J -2 10 0 FUNCTIONAL 10 0 0
[OUTFALLS]
O 0 FREE NO
[CONDUITS]
C J O 5 0.013 0 0
[XSECTIONS]
C CIRCULAR 1 0 0 0 2
[2D_OPTIONS]
BACKEND CPU
REPORT_2D NO
MAX_TIMESTEP 1
LTS_TIERS 4
COUPLING_SYNC {sync}
[2D_VERTICES]
0 0 0
1 0 0
0 1 0
-1 0 0
0 -1 0
[2D_TRIANGLES]
0 1 2 .03 .2
0 3 4 .03 2
[2D_VERTEX_NODE_MAP]
0 O .7 1
''')
  e=create()
  def check(code):
   if code: raise RuntimeError((tag,code))
  try:
   check(open_(e,str(inp).encode(),str(rpt).encode(),None,None));check(init(e));check(start(e,0))
   t=D();steps=0
   while True:
    check(step(e,c.byref(t)));steps+=1
    if t.value<=0:break
    if steps>100000:raise RuntimeError('step guard')
   check(end(e));vals=[D() for _ in range(10)];check(mb(e,*[c.byref(x) for x in vals]));e2=D();e1=D();check(err(e,c.byref(e2)));check(err1(e,c.byref(e1)))
   check(report(e))
   results.append(dict(case=tag,steps=steps,initial=vals[0].value,stored=vals[1].value,outfall_in=vals[5].value,outfall_out=vals[6].value,continuity_2d=e2.value,continuity_1d=e1.value))
  finally:close(e);destroy(e)
print(json.dumps(results,indent=2));(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
