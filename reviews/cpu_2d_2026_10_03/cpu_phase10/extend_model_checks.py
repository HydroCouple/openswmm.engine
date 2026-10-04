from pathlib import Path
p=Path(__file__).resolve().parent;f=p/'run_engine.py';s=f.read_text().replace('import ctypes as C,sys,time,json,hashlib,resource,os','import ctypes as C,sys,time,json,hashlib,resource,os,math')
pos=s.index('cpu_start=')
s=s[:pos]+'''class RunStats(C.Structure):
 _fields_=[('backend',C.c_char*64),('momentum',integer),('lts_tiers',integer),('steps',C.c_long),('face_evals',C.c_long),('last_step',dbl),('active_frac_min',dbl),('active_frac_mean',dbl),('active_frac_max',dbl),('n_tiers',integer),('tier_cells',C.c_long*8)]
def telemetry(e):
 st=RunStats();lib.swmm_2d_get_run_stats.argtypes=[ptr,C.POINTER(RunStats)];check(lib.swmm_2d_get_run_stats(e,C.byref(st)))
 return dict(backend=st.backend.decode(),momentum=st.momentum,lts_tiers=st.lts_tiers,steps=st.steps,face_evals=st.face_evals,active_frac_mean=st.active_frac_mean)
'''+s[pos:]
s=s.replace('gw=groundwater(e);check(lib.swmm_engine_end(e))','gw=groundwater(e);stats=telemetry(e);assert all(math.isfinite(x) and x>=0 for x in depth);check(lib.swmm_engine_end(e))').replace('dict(groundwater=gw,cpu_seconds=', 'dict(stats=stats,groundwater=gw,cpu_seconds=')
f.write_text(s)
f=p/'check_models.py';s=f.read_text().replace("row=json.loads(next(x for x in result.stdout.splitlines()if x.startswith('{')))","row=json.loads(next(x for x in result.stdout.splitlines()if x.startswith('{')))\n  if 'swe2' in deck.name: assert row['stats']['momentum']==1 and row['stats']['lts_tiers']==1 and row['stats']['face_evals']>0")
f.write_text(s)
