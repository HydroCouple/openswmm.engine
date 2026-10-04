from pathlib import Path
import ctypes as C,sys,time,json,hashlib,resource,os,math
lib=C.CDLL(str(Path(sys.argv[1]).resolve()));inp=Path(sys.argv[2]).resolve();out=Path(sys.argv[3]).resolve();out.mkdir(exist_ok=True,parents=True)
# Generated output must stay in this run's own directory.
model=out/'model.inp';model.write_text(inp.read_text())
ptr=C.c_void_p;dbl=C.c_double;integer=C.c_int
lib.swmm_engine_create.restype=ptr
for name in ['initialize','end','report','close','destroy']:
 getattr(lib,'swmm_engine_'+name).argtypes=[ptr]
lib.swmm_engine_open.argtypes=[ptr,C.c_char_p,C.c_char_p,C.c_char_p,C.c_char_p];lib.swmm_engine_start.argtypes=[ptr,integer];lib.swmm_engine_step.argtypes=[ptr,C.POINTER(dbl)]
lib.swmm_2d_triangle_count.argtypes=[ptr,C.POINTER(integer)];lib.swmm_2d_get_depths_bulk.argtypes=[ptr,C.POINTER(dbl)];lib.swmm_2d_get_mass_balance.argtypes=[ptr]+[C.POINTER(dbl)]*10
lib.swmm_2d_get_continuity_error.argtypes=[ptr,C.POINTER(dbl)]
lib.swmm_infil2d_get_total_volume.argtypes=[ptr,C.POINTER(dbl)]
def groundwater(e):
 active=integer();lib.swmm_gw2d_is_active.argtypes=[ptr,C.POINTER(integer)];check(lib.swmm_gw2d_is_active(e,C.byref(active)))
 if not active.value:return dict(active=False)
 lib.swmm_gw2d_get_dimensions.argtypes=[ptr,C.POINTER(integer),C.POINTER(integer)]
 nc=integer();layers=integer();check(lib.swmm_gw2d_get_dimensions(e,C.byref(nc),C.byref(layers)))
 lib.swmm_gw2d_get_cell_bulk.argtypes=[ptr,integer,C.POINTER(dbl),integer,C.POINTER(integer)]
 h=hashlib.sha256()
 for var in range(14):
  a=(dbl*nc.value)();written=integer();check(lib.swmm_gw2d_get_cell_bulk(e,var,a,nc.value,C.byref(written)));assert written.value==nc.value;h.update(bytes(a))
 lib.swmm_gw2d_get_ledger.argtypes=[ptr,integer,C.POINTER(dbl)];ledger=[]
 for term in range(13):
  x=dbl();check(lib.swmm_gw2d_get_ledger(e,term,C.byref(x)));ledger.append(x.value)
 lib.swmm_gw2d_get_continuity_error.argtypes=[ptr,C.POINTER(dbl)];error=dbl();check(lib.swmm_gw2d_get_continuity_error(e,C.byref(error)))
 lib.swmm_gw2d_species_count.argtypes=[ptr,C.POINTER(integer)];ns=integer();check(lib.swmm_gw2d_species_count(e,C.byref(ns)))
 lib.swmm_gw2d_get_cell_conc.argtypes=[ptr,integer,integer,C.POINTER(dbl),integer,C.POINTER(integer)]
 lib.swmm_gw2d_get_species_ledger.argtypes=[ptr,integer,integer,C.POINTER(dbl)];species=[]
 for row in range(ns.value):
  for zone in [0,1]:
   a=(dbl*nc.value)();written=integer();check(lib.swmm_gw2d_get_cell_conc(e,zone,row,a,nc.value,C.byref(written)));assert written.value==nc.value;h.update(bytes(a))
  terms=[]
  for term in range(15):
   x=dbl();check(lib.swmm_gw2d_get_species_ledger(e,row,term,C.byref(x)));terms.append(x.value)
  species.append(terms)
 return dict(active=True,cells=nc.value,layers=layers.value,hash=h.hexdigest(),ledger=ledger,residual=error.value,species=species)
class RunStats(C.Structure):
 _fields_=[('backend',C.c_char*64),('momentum',integer),('lts_tiers',integer),('steps',C.c_long),('face_evals',C.c_long),('last_step',dbl),('active_frac_min',dbl),('active_frac_mean',dbl),('active_frac_max',dbl),('n_tiers',integer),('tier_cells',C.c_long*8)]
def telemetry(e):
 st=RunStats();lib.swmm_2d_get_run_stats.argtypes=[ptr,C.POINTER(RunStats)];check(lib.swmm_2d_get_run_stats(e,C.byref(st)))
 return dict(backend=st.backend.decode(),momentum=st.momentum,lts_tiers=st.lts_tiers,steps=st.steps,face_evals=st.face_evals,active_frac_mean=st.active_frac_mean)
cpu_start=resource.getrusage(resource.RUSAGE_SELF);start=time.perf_counter();e=lib.swmm_engine_create();codes=[]
def check(code):
 codes.append(code)
 if code:
  lib.swmm_get_last_error_msg.argtypes=[ptr];lib.swmm_get_last_error_msg.restype=C.c_char_p
  raise RuntimeError(f'Engine returned {code}: {lib.swmm_get_last_error_msg(e)!r}; see {out}')
try:
 check(lib.swmm_engine_open(e,os.fsencode(model),os.fsencode(out/'model.rpt'),os.fsencode(out/'model.out'),None));check(lib.swmm_engine_initialize(e));check(lib.swmm_engine_start(e,1));init=time.perf_counter()-start
 loop=time.perf_counter();elapsed=dbl();steps=0
 while True:
  check(lib.swmm_engine_step(e,C.byref(elapsed)));steps+=1
  if elapsed.value<=0:break
 run_seconds=time.perf_counter()-loop;n=integer();check(lib.swmm_2d_triangle_count(e,C.byref(n)));depth=(dbl*n.value)();check(lib.swmm_2d_get_depths_bulk(e,depth));terms=[dbl()for _ in range(10)];check(lib.swmm_2d_get_mass_balance(e,*[C.byref(x)for x in terms]));err=dbl();check(lib.swmm_2d_get_continuity_error(e,C.byref(err)))
 infil=dbl();check(lib.swmm_infil2d_get_total_volume(e,C.byref(infil)));state_hash=hashlib.sha256(bytes(depth)).hexdigest();gw=groundwater(e);stats=telemetry(e);assert all(math.isfinite(x) and x>=0 for x in depth);check(lib.swmm_engine_end(e));check(lib.swmm_engine_report(e));check(lib.swmm_engine_close(e));total=time.perf_counter()-start
 cpu_end=resource.getrusage(resource.RUSAGE_SELF);cpu_seconds=(cpu_end.ru_utime-cpu_start.ru_utime)+(cpu_end.ru_stime-cpu_start.ru_stime)
 print(json.dumps(dict(stats=stats,groundwater=gw,cpu_seconds=cpu_seconds,cells=n.value,init_seconds=init,run_seconds=run_seconds,total_seconds=total,routing_steps=steps,depth_hash=state_hash,mass_balance=[x.value for x in terms],infiltration=infil.value,continuity_error=err.value,peak_rss_bytes=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)))
finally:lib.swmm_engine_destroy(e)
