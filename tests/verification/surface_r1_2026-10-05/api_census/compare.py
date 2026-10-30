from pathlib import Path
import ctypes as c,sys,struct,hashlib,json,subprocess,os
ROOT=Path(__file__).resolve().parent
if len(sys.argv)>1 and sys.argv[1]=='worker':
 _,_,library,deck,target=sys.argv
 out=Path(target);out.mkdir(parents=True,exist_ok=True)
 lib=c.CDLL(library);H=c.c_void_p;D=c.POINTER(c.c_double)
 signatures={'swmm_engine_create':([],H),'swmm_engine_destroy':([H],None),'swmm_engine_open':([H,c.c_char_p,c.c_char_p,c.c_char_p,c.c_char_p],c.c_int),'swmm_engine_initialize':([H],c.c_int),'swmm_engine_start':([H,c.c_int],c.c_int),'swmm_engine_step':([H,D],c.c_int),'swmm_engine_end':([H],c.c_int),'swmm_engine_close':([H],c.c_int),'swmm_2d_cell_count':([H,c.POINTER(c.c_int)],c.c_int),'swmm_2d_get_depths_bulk':([H,D],c.c_int),'swmm_infil2d_get_rate_bulk':([H,D,c.c_int],c.c_int),'swmm_infil2d_get_cum_bulk':([H,D,c.c_int],c.c_int),'swmm_infil2d_get_total_volume':([H,D],c.c_int),'swmm_2d_triangle_get_area':([H,c.c_int,D],c.c_int),'swmm_2d_force_rainfall_uniform':([H,c.c_double,c.c_int,c.c_int],c.c_int)}
 for name,(args,ret) in signatures.items():fn=getattr(lib,name);fn.argtypes=args;fn.restype=ret
 h=lib.swmm_engine_create();assert h
 def check(value):assert value==0,value
 check(lib.swmm_engine_open(h,str(Path(deck).resolve()).encode(),str(out/'run.rpt').encode(),str(out/'run.out').encode(),None));check(lib.swmm_engine_initialize(h));check(lib.swmm_engine_start(h,1))
 n=c.c_int();check(lib.swmm_2d_cell_count(h,c.byref(n)));assert n.value>0
 a=(c.c_double*n.value)();v=c.c_double();elapsed=c.c_double();t=0.;samples=0
 check(lib.swmm_infil2d_get_rate_bulk(h,a,n.value));initial=list(a)
 areas=[]
 for i in range(n.value):check(lib.swmm_2d_triangle_get_area(h,i,c.byref(v)));areas.append(v.value)
 with (out/'series.bin').open('wb') as f:
  for step in range(20000):
   rain=3e-5 if t<float(os.environ.get('SURFACE_R1_WET_SECONDS','1800')) or t>=float(os.environ.get('SURFACE_R1_SECOND_WET_SECONDS','5400')) else 0
   check(lib.swmm_2d_force_rainfall_uniform(h,rain,1,1));check(lib.swmm_engine_step(h,c.byref(elapsed)))
   f.write(struct.pack('d',elapsed.value))
   for getter in ['swmm_2d_get_depths_bulk','swmm_infil2d_get_rate_bulk','swmm_infil2d_get_cum_bulk']:
    check(getattr(lib,getter)(h,a) if getter=='swmm_2d_get_depths_bulk' else getattr(lib,getter)(h,a,n.value));f.write(bytes(a))
   check(lib.swmm_infil2d_get_total_volume(h,c.byref(v)));f.write(struct.pack('d',v.value));samples+=1
   if elapsed.value==0:break
   t=elapsed.value*86400 # API elapsed days
  else:raise AssertionError('unfinished run')
 check(lib.swmm_infil2d_get_cum_bulk(h,a,n.value))
 applied=sum(x*area for x,area in zip(a,areas));assert abs(applied-v.value)<=1e-10*max(1.,abs(v.value)),(applied,v.value)
 check(lib.swmm_engine_end(h));check(lib.swmm_engine_close(h));lib.swmm_engine_destroy(h)
 (out/'stats.json').write_text(json.dumps({'samples':samples,'cells':n.value,'volume_m3':v.value,'applied_integral_m3':applied,'initial_rate_m_s':initial,'series_sha256':hashlib.sha256((out/'series.bin').read_bytes()).hexdigest()},indent=2));sys.exit(0)
base,candidate=sys.argv[1:3]
template=(ROOT.parent/'isolated/source/tests/unit/engine/data/infil2d_out/g3_on.inp').read_text()
methods={'HORTON':[3,.4,4,.1,10],'MODIFIED_HORTON':[3,.4,4,.1,10],'GREEN_AMPT':[4,.5,.3,0,0],'MODIFIED_GREEN_AMPT':[4,.5,.3,0,0],'CURVE_NUMBER':[75,0,.1,0,0],'CONSTANT':[.5,0,0,0,0]}
rows=[]
for si in [False,True]:
 for method,p0 in methods.items():
  name=method.lower()+('_si' if si else '_us');p=p0.copy()
  if si:
   slots=[0,1,4] if 'HORTON' in method else [0,1] if 'AMPT' in method else [0] if method=='CONSTANT' else []
   for i in slots:p[i]*=25.4
  deck=ROOT/(name+'.inp');body=template[:template.index('[2D_INFILTRATION_DEFAULTS]')]
  body=body.replace('FLOW_UNITS           CFS','FLOW_UNITS           '+('CMS' if si else 'CFS'))
  body+='[2D_INFILTRATION_DEFAULTS]\n* '+method+' '+' '.join(map(str,p))+' LOST\n'
  deck.write_text(body);stats=[]
  for side,lib in [('baseline',base),('candidate',candidate)]:
   target=ROOT/side/name;target.mkdir(parents=True,exist_ok=True)
   with (target/'stdout.log').open('w') as f:subprocess.run([sys.executable,__file__,'worker',str(Path(lib).resolve()),str(deck),str(target)],check=True,stdout=f,stderr=subprocess.STDOUT)
   stats.append(json.loads((target/'stats.json').read_text()))
  equal=stats[0]['series_sha256']==stats[1]['series_sha256'];assert equal,(name,stats)
  rows.append({'deck':deck.name,'deck_sha256':hashlib.sha256(deck.read_bytes()).hexdigest(),'bit_identical':equal,'baseline':stats[0],'candidate':stats[1]})
(ROOT/'summary.json').write_text(json.dumps(rows,indent=2));print(len(rows),'wet/dry/wet API trajectories byte-identical')
