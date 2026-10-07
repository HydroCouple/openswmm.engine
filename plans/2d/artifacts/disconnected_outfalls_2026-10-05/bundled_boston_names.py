from pathlib import Path
import json,ctypes,collections
root=Path.cwd(); artifacts=root.parent/'openswmm.engine/plans/2d/artifacts/disconnected_outfalls_2026-10-05'
audit=json.loads((root/'workplans/artifacts/boston_outfall_init_2026-10-05/model_audit.json').read_text())
names=[item['name'] for item in audit['isolated_outfalls']]
model=(root/'workplans/artifacts/boston_outfall_init_2026-10-05/unconnected_uncoupled.inp').read_text()
model=model.replace('UNCONNECTED 0 FREE NO\n',''.join(name+' 0 FREE NO\n' for name in names))
# Several mappings to one cell remain authored; the runtime coupling set skips
# them all. Preserve one connected O outfall as an active coupling control.
model+='[2D_TRIANGLE_NODE_MAP]\n'+''.join('0 '+name+' 0.65 1\n' for name in names)
model+='1 '+names[0]+' 0.65 1\n' # one duplicate to verify warning de-duplication
inp=artifacts/'boston_disconnected_names.inp';inp.write_text(model)
libpath=root/'build/SWMMVis.app/Contents/Frameworks/libopenswmm.engine.6.dylib'; lib=ctypes.CDLL(str(libpath))
lib.swmm_engine_create.restype=ctypes.c_void_p
for name,args in [('swmm_engine_open',[ctypes.c_void_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p]),('swmm_engine_initialize',[ctypes.c_void_p]),('swmm_engine_close',[ctypes.c_void_p]),('swmm_engine_destroy',[ctypes.c_void_p]),('swmm_get_warning_count',[ctypes.c_void_p]),('swmm_get_warning_at',[ctypes.c_void_p,ctypes.c_int]),('swmm_get_error_count',[ctypes.c_void_p])]: getattr(lib,name).argtypes=args
lib.swmm_get_warning_at.restype=ctypes.c_char_p
engine=lib.swmm_engine_create()
try:
 opened=lib.swmm_engine_open(engine,str(inp).encode(),str(artifacts/'boston_disconnected_names.rpt').encode(),None,None);assert opened==0,opened
 initialized=lib.swmm_engine_initialize(engine);assert initialized==0,initialized
 warnings=[lib.swmm_get_warning_at(engine,i).decode() for i in range(lib.swmm_get_warning_count(engine))]
 ignored=[w for w in warnings if '2D coupling ignored for outfall' in w]
 assert len(ignored)==17,ignored
 for name in names: assert sum("outfall '"+name+"'" in w for w in ignored)==1,(name,ignored)
 assert lib.swmm_get_error_count(engine)==0
 result={'library':str(libpath),'fixture':str(inp),'initialize_code':initialized,'ignored_outfall_count':len(ignored),'warnings':ignored,'duplicate_mapping_warns_once':True,'uses_full_boston_model':False}
 (artifacts/'bundled_names_result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
finally:
 lib.swmm_engine_close(engine);lib.swmm_engine_destroy(engine)
