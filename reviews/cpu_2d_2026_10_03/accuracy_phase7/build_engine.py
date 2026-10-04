from pathlib import Path
import shutil,json,subprocess,shlex
p=Path(__file__).resolve().parent;q=p.parent/'cpu_phase5';root=p.parents[2];b=root/'build/flow-trace'
# All non-solver 2D files are identical to phase 5. Reuse its frozen support
# source/objects, avoiding unrelated concurrent 1D changes and build activity.
base=p/'engine_src_baseline';sel=p/'engine_src_selected'
for dest in [base,sel]:
 if not dest.exists():shutil.copytree(q/'src_selected',dest)
for name in ['ExplicitInertialSolver.cpp','ExplicitInertialSolver.hpp','SweKernels.hpp']:
 shutil.copyfile(p/'src_selected/src/engine/2d/solver'/name,sel/'src/engine/2d/solver'/name)
# Remove review-only public accessors from the full-engine candidate.
h=sel/'src/engine/2d/solver/ExplicitInertialSolver.hpp';hs=h.read_text().replace('public:\n    const std::vector<double>& reviewQx()const{return qcx_;}\n    const std::vector<double>& reviewQy()const{return qcy_;}\nprivate:', 'private:');h.write_text(hs)
(p/'.gitignore').write_text((p/'.gitignore').read_text()+'/engine_src_*/\n')
config=json.loads((q/'engine_commands.json').read_text());target='src/engine/libopenswmm.engine.6.0.0.dylib';lines=subprocess.check_output(['/opt/homebrew/bin/ninja','-C',str(b),'-t','commands',target],text=True).splitlines();commands=[]
objects={}
for rel in ['2d/solver/ExplicitInertialSolver.cpp']:
 command=next(shlex.split(s)for s in lines if ' -c 'in s and s.endswith('/src/engine/'+rel));old=command[command.index('-o')+1];obj=str(p/'build_selected'/('engine_'+Path(rel).with_suffix('.o').name))
 cmd=[x.replace(str(root/'src/engine'),str(sel/'src/engine')).replace(str(root/'include'),str(sel/'include'))for x in command];cmd[cmd.index('-o')+1]=obj;cmd[cmd.index('-MF')+1]=obj+'.d';cmd[cmd.index('-MT')+1]=obj;subprocess.run(cmd,cwd=b,check=True);commands.append(cmd);objects[old]=obj
link=[]
for x in config['link']:
 if x in objects:x=objects[x]
 elif x==target:x=str(p/'build_selected/libopenswmm.engine.selected.dylib')
 elif x.endswith(('.o','.a')):x=str(q/'engine_common'/x.lstrip('/'))
 link.append(x)
subprocess.run(link,cwd=b,check=True);commands.append(link)
for v,lib in [('selected',p/'build_selected/libopenswmm.engine.selected.dylib'),('baseline',q/'build_selected/libopenswmm.engine.selected.dylib')]:
 alias=p/f'build_{v}/libopenswmm.engine.6.dylib'
 if not alias.exists():alias.symlink_to(lib)
(p/'engine_commands.json').write_text(json.dumps(commands,indent=2))
# Recompile each suite against the frozen support headers and candidate solver.
rows=json.loads((q/'regression_results.json').read_text());tests=[]
for row in rows:
 name=row['suite'];lines=subprocess.check_output(['/opt/homebrew/bin/ninja','-C',str(b),'-t','commands',name],text=True).splitlines()
 compile=next(shlex.split(s)for s in lines if ' -c 'in s and '/tests/unit/engine/'in s and s.endswith('.cpp'))
 source=Path(compile[-1]);copy=p/'test_sources'/source.name;copy.parent.mkdir(exist_ok=True);text=source.read_text()
 if name=='test_engine_2d_cpu_correctness':
  text=text.replace('    static void rebuild(ExplicitInertialSolver& s)', '    static void fireSwe(ExplicitInertialSolver& s,double dt) {s.fireFacesSwe(s.active_faces_,dt,true);}\n    static void rebuild(ExplicitInertialSolver& s)',1)+(p/'new_tests.cpp').read_text()
 copy.write_text(text)
 for v in (['selected','baseline']if name=='test_engine_2d_cpu_correctness'else ['selected']):
  src=sel if v=='selected'else base;old=compile[compile.index('-o')+1];obj=str(p/f'build_{v}'/(source.stem+'.o'));cmd=[x.replace(str(root/'src/engine'),str(src/'src/engine')).replace(str(root/'include'),str(src/'include'))for x in compile];cmd[cmd.index('-o')+1]=obj;cmd[cmd.index('-MF')+1]=obj+'.d';cmd[cmd.index('-MT')+1]=obj;cmd[-1]=str(copy);cmd.insert(1,'-I'+str(source.parent));subprocess.run(cmd,cwd=b,check=True)
  l=shlex.split(lines[-1])[2:-2];l=[obj if x==old else str(p/f'build_{v}'/name)if x=='bin/Release/'+name else str(p/'build_selected/libopenswmm.engine.selected.dylib')if x==target and v=='selected'else str(q/'build_selected/libopenswmm.engine.selected.dylib')if x==target else x for x in l];subprocess.run(l,cwd=b,check=True);tests.extend([cmd,l])
 print('built',name,flush=True)
(p/'test_commands.json').write_text(json.dumps(tests,indent=2));print('Engine and all regression suites rebuilt against matched private layouts',flush=True)
