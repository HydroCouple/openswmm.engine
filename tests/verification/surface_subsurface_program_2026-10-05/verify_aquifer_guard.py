"""Check that unsupported Richards/aquifer beds fail before simulation.
The explicit EXCHANGE NO opt-out retains native-soil bottom behavior.
"""
from pathlib import Path
import ctypes as C
import json
import sys
folder=Path(__file__).resolve().parent
root=folder.parents[2]
lib=C.CDLL(str(root/'build/surface-program/src/engine/libopenswmm.engine.6.0.0.dylib'))
lib.swmm_engine_create.restype=C.c_void_p
lib.swmm_engine_open.argtypes=[C.c_void_p,C.c_char_p,C.c_char_p,C.c_char_p,C.c_char_p]
for name in ('initialize','close','destroy'):
 getattr(lib,'swmm_engine_'+name).argtypes=[C.c_void_p]
lib.swmm_engine_start.argtypes=[C.c_void_p,C.c_int]
lib.swmm_engine_end.argtypes=[C.c_void_p]
lib.swmm_get_last_error_msg.argtypes=[C.c_void_p]
lib.swmm_get_last_error_msg.restype=C.c_char_p
base=(root/'tests/output/aquifer_2d/storage_bed.inp').read_text()
richards='''
[LID_CONTROLS]
Stack NODE
Stack SURFACE 200 .1
Stack MEDIA 1300 .45 .2 .08 10 10 100
Stack AGGREGATE 500 .4 1000
Stack BOTTOM .5 0
[LID_NODES]
ST1 Stack 10
[LID_RICHARDS]
Stack OPTIONS 4 1e-7 1e-5 30
Stack 2 .03 2 1.6 .5 .0001
Stack 3 .03 2 1.6 .5 .0001
'''
records=[]
for optout in (False,True):
 stem=folder/('aquifer_optout' if optout else 'aquifer_guard')
 inp=stem.with_suffix('.inp'); inp.write_text(base+richards+('\n[2D_AQUIFER_NODE]\nST1 AUTO EXCHANGE NO\n' if optout else ''))
 e=lib.swmm_engine_create()
 try:
  opened=lib.swmm_engine_open(e,str(inp).encode(),str(stem.with_suffix('.rpt')).encode(),None,None)
  initialized=lib.swmm_engine_initialize(e) if opened==0 else opened
  started=lib.swmm_engine_start(e,0) if initialized==0 else initialized
  message=lib.swmm_get_last_error_msg(e).decode()
  records.append(dict(optout=optout,opened=opened,initialized=initialized,started=started,error=message))
  if optout:
   assert started==0,records[-1]
   lib.swmm_engine_end(e)
  else:
   assert started!=0 and 'Richards' in message and 'aquifer' in message,records[-1]
 finally:
  lib.swmm_engine_close(e);lib.swmm_engine_destroy(e)
(folder/'aquifer_guard.json').write_text(json.dumps(records,indent=2)+'\n')
print(json.dumps(records,indent=2))
