from pathlib import Path
import json,subprocess,shutil
p=Path(__file__).resolve().parent;q=p.parent/'accuracy_phase7';root=p.parents[2];b=root/'build/flow-trace';commands=[]
for v in ['selected','connected_face']:
 src=p/f'engine_src_{v}';shutil.copytree(q/'engine_src_selected',src,dirs_exist_ok=True)
 f='src/engine/2d/solver/ExplicitInertialSolver.cpp';shutil.copy(p/f'src_{v}'/f,src/f)
 out=p/f'build_{v}';obj=str(out/'engine_ExplicitInertialSolver.o')
 cmd=json.loads((q/'engine_commands.json').read_text())[0]
 cmd=[x.replace(str(q/'engine_src_selected'),str(src))for x in cmd]
 for flag in ['-o','-MT']:cmd[cmd.index(flag)+1]=obj
 cmd[cmd.index('-MF')+1]=obj+'.d';subprocess.run(cmd,cwd=b,check=True);commands.append(cmd)
 cmd=json.loads((q/'support_link_commands.json').read_text())[1]
 lib=str(out/f'libopenswmm.engine.{v}.dylib');cmd=[str(q/'engine_common/geo/libopenswmm.geopackage.a')if x.endswith('libopenswmm.geopackage.a')else obj if x.endswith('/build_selected/ExplicitInertialSolver.o')else lib if x==str(q/'build_selected/libopenswmm.engine.selected.dylib')else x for x in cmd]
 assert obj in cmd and lib in cmd;subprocess.run(cmd,cwd=b,check=True);commands.append(cmd)
 alias=out/'libopenswmm.engine.6.dylib'
 if alias.is_symlink():alias.unlink()
 alias.symlink_to(Path(lib).name)
# Baseline alias uses the final phase-7 matched-support library.
alias=p/'build_baseline/libopenswmm.engine.6.dylib'
if alias.is_symlink():alias.unlink()
alias.symlink_to(q/'build_selected/libopenswmm.engine.selected.dylib')
# One binary, forced to the requested private library per run.
cmds=json.loads((q/'test_commands.json').read_text());cmds=[c for c in cmds if str(q/'build_selected/test_2d_cpu_correctness.o')in c];assert len(cmds)==2
for c in cmds:
 c=[x.replace(str(q/'build_selected/test_2d_cpu_correctness.o'),str(p/'build_selected/test_2d_cpu_correctness.o')).replace(str(q/'build_selected/test_engine_2d_cpu_correctness'),str(p/'build_selected/test_engine_2d_cpu_correctness')).replace(str(q/'test_sources/test_2d_cpu_correctness.cpp'),str(p/'test_sources/test_2d_cpu_correctness.cpp')).replace(str(q/'build_selected/libopenswmm.engine.selected.dylib'),str(p/'build_selected/libopenswmm.engine.selected.dylib'))for x in c]
 subprocess.run(c,cwd=b,check=True);commands.append(c)
(p/'engine_commands.json').write_text(json.dumps(commands,indent=2))
