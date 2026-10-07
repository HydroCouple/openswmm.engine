from pathlib import Path
import json,subprocess,concurrent.futures
p=Path(__file__).resolve().parent;q=p.parent/'cpu_phase5';root=p.parents[2];b=root/'build/flow-trace';cfg=json.loads((q/'engine_commands.json').read_text());src=p/'engine_src_baseline';out=p/'engine_common';out.mkdir(exist_ok=True);commands=[];mapping={}
extra='src/engine/CMakeFiles/openswmm_engine.dir/hydrology/LidNode.cpp.o'
for old in cfg['link']+[extra]:
 if not old.endswith('.o'):continue
 assert '/CMakeFiles/openswmm_engine.dir/'in old,old
 rel=old.split('/CMakeFiles/openswmm_engine.dir/')[1][:-2];source=src/'src/engine'/rel;assert source.exists(),source;obj=out/(rel+'.o');obj.parent.mkdir(exist_ok=True,parents=True)
 cmd=[x.replace(str(root/'src/engine'),str(src/'src/engine')).replace(str(root/'include'),str(src/'include'))for x in cfg['compile']];cmd[cmd.index('-o')+1]=str(obj);cmd[cmd.index('-MF')+1]=str(obj)+'.d';cmd[cmd.index('-MT')+1]=str(obj);cmd[-1]=str(source);commands.append(cmd);mapping[old]=str(obj)
(p/'support_commands.json').write_text(json.dumps(commands,indent=2))
def compile(cmd):
 if Path(cmd[cmd.index('-o')+1]).exists():return cmd[-1]
 r=subprocess.run(cmd,cwd=b,capture_output=True,text=True)
 if r.returncode:raise RuntimeError(r.stdout+r.stderr)
 return cmd[-1]
with concurrent.futures.ThreadPoolExecutor(max_workers=4)as pool:
 for i,f in enumerate(pool.map(compile,commands)):
  if(i+1)%10==0:print('compiled',i+1,len(commands),flush=True)
# Rebuild selected solver against the matching source support snapshot.
cmd=json.loads((p/'engine_commands.json').read_text())[0];subprocess.run(cmd,cwd=b,check=True)
links=[]
for v in ['baseline','selected']:
 link=[]
 for x in cfg['link']+[extra]:
  if x=='src/engine/libopenswmm.engine.6.0.0.dylib':x=str(p/f'build_{v}/libopenswmm.engine.{v}.dylib')
  elif x.endswith('/2d/solver/ExplicitInertialSolver.cpp.o')and v=='selected':x=cmd[cmd.index('-o')+1]
  elif x in mapping:x=mapping[x]
  elif x.endswith('.a'):x=str(q/'engine_common'/x.lstrip('/'))
  link.append(x)
 subprocess.run(link,cwd=b,check=True);links.append(link)
 alias=p/f'build_{v}/libopenswmm.engine.6.dylib'
 if alias.is_symlink():alias.unlink()
 alias.symlink_to(p/f'build_{v}/libopenswmm.engine.{v}.dylib')
(p/'support_link_commands.json').write_text(json.dumps(links,indent=2));print('Full matched baseline and selected engine support rebuilt',flush=True)
