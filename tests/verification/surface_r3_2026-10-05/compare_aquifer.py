from pathlib import Path
import ctypes as c,subprocess,sys,json,hashlib
root=Path.cwd();out=root/'tests/verification/surface_r3_2026-10-05';dest=out/'aquifer_changes';dest.mkdir(exist_ok=True)
if len(sys.argv)>1 and sys.argv[1]=='worker':
 lib=c.CDLL(sys.argv[2]);deck=Path(sys.argv[3]);folder=Path(sys.argv[4]);folder.mkdir(parents=True,exist_ok=True);H=c.c_void_p;D=c.POINTER(c.c_double)
 sig={'swmm_engine_create':([],H),'swmm_engine_destroy':([H],None),'swmm_engine_open':([H,c.c_char_p,c.c_char_p,c.c_char_p,c.c_char_p],c.c_int),'swmm_engine_initialize':([H],c.c_int),'swmm_engine_start':([H,c.c_int],c.c_int),'swmm_engine_step':([H,D],c.c_int),'swmm_engine_end':([H],c.c_int),'swmm_engine_close':([H],c.c_int),'swmm_gw2d_get_ledger':([H,c.c_int,D],c.c_int),'swmm_gw2d_get_continuity_error':([H,D],c.c_int),'swmm_2d_get_depths_bulk':([H,D],c.c_int),'swmm_2d_cell_count':([H,c.POINTER(c.c_int)],c.c_int)}
 for name,(args,ret) in sig.items():fn=getattr(lib,name);fn.argtypes=args;fn.restype=ret
 def check(x):assert x==0,x
 h=lib.swmm_engine_create();check(lib.swmm_engine_open(h,str(deck).encode(),str(folder/'run.rpt').encode(),str(folder/'run.out').encode(),None));check(lib.swmm_engine_initialize(h));check(lib.swmm_engine_start(h,0));elapsed=c.c_double()
 for n in range(10000):
  check(lib.swmm_engine_step(h,c.byref(elapsed)))
  if elapsed.value==0:break
 else:raise AssertionError('unfinished model')
 metrics={};v=c.c_double()
 for name,i in {'soil_et_m3':6,'caprise_m3':5,'node_m3':3,'link_m3':10,'storage_m3':9}.items():check(lib.swmm_gw2d_get_ledger(h,i,c.byref(v)));metrics[name]=v.value
 check(lib.swmm_gw2d_get_continuity_error(h,c.byref(v)));metrics['residual_m3']=v.value
 n=c.c_int();check(lib.swmm_2d_cell_count(h,c.byref(n)));a=(c.c_double*n.value)();check(lib.swmm_2d_get_depths_bulk(h,a));metrics['depths_m']=list(a);check(lib.swmm_engine_end(h));check(lib.swmm_engine_close(h));lib.swmm_engine_destroy(h);(folder/'metrics.json').write_text(json.dumps(metrics,indent=2)+'\n');sys.exit(0)
rows=[];fixtures=[]
for closure in ['CLOSED_FORM','SIGMA','ENSLAVED']:
 for water in ['dry','ponded']:
  source=out/'models'/f'et_{closure}_{water}.inp';body=source.read_text().replace('REPORT_2D YES','REPORT_2D NO')
  fixtures.append((f'automatic_{closure}_{water}',body,'Automatic mesh ET resolves BOTH; single shared demand.'))
  fixtures.append((f'old_opt_out_{closure}_{water}',body.replace('[2D_AQUIFER_OPTIONS]','[2D_AQUIFER_OPTIONS]\nGW_ET NONE\nLINK_SEEPAGE AUTO'),'Explicit legacy opt-outs retain behavior.'))
  fixtures.append((f'both_{closure}_{water}',body.replace('[2D_AQUIFER_OPTIONS]','[2D_AQUIFER_OPTIONS]\nGW_ET BOTH\nLINK_SEEPAGE AUTO'),'Surface-first demand and a single stress/threshold replace duplicate soil withdrawal.'))
source=root/'tests/output/aquifer_2d/default_signed.inp';body=source.read_text().replace('[2D_AQUIFER_OPTIONS]','[2D_AQUIFER_OPTIONS]\nGW_ET NONE')
fixtures.append(('automatic_signed_link',body,'Absent LINK_SEEPAGE resolves the signed TWO_WAY law; this high-table conduit gains.'))
fixtures.append(('explicit_legacy_link',body.replace('[2D_AQUIFER_OPTIONS]','[2D_AQUIFER_OPTIONS]\nLINK_SEEPAGE AUTO'),'Explicit legacy AUTO retains one-way delivery.'))
for name,body,reason in fixtures:
 path=dest/(name+'.inp');path.write_text(body);values={}
 for side in ['baseline','candidate']:
  folder=dest/side/name;folder.mkdir(parents=True,exist_ok=True);lib=out/'isolated'/side/'libopenswmm.engine.6.0.0.dylib'
  with (folder/'stdout.log').open('w') as log:subprocess.run([sys.executable,__file__,'worker',str(lib),str(path),str(folder)],check=True,stdout=log,stderr=subprocess.STDOUT)
  values[side]=json.loads((folder/'metrics.json').read_text())
 changed=values['baseline']!=values['candidate'];row={'deck':path.name,'input_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'changed':changed,'reason':reason,**values};rows.append(row);print(name,changed,flush=True)
 if name.startswith('old_opt_out') or name=='explicit_legacy_link':assert not changed,row
 if name.startswith('both_') and name.endswith('ponded'):assert values['baseline']['soil_et_m3']>0 and values['candidate']['soil_et_m3']==0,row
 if name.startswith('automatic_') and name.endswith('_dry'):assert values['baseline']['soil_et_m3']==0 and values['candidate']['soil_et_m3']>0,row
 if name=='automatic_signed_link':assert values['baseline']['link_m3']>0 and values['candidate']['link_m3']<0,row
 for side in ['baseline','candidate']:assert abs(values[side]['residual_m3'])<1e-10*max(1.,values[side]['storage_m3']),row
(dest/'manifest.json').write_text(json.dumps(rows,indent=2)+'\n');print(sum(r['changed'] for r in rows),'named intentional changes;',len(rows),'cases',flush=True)
