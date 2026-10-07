from pathlib import Path
import subprocess,json,sys
p=Path(__file__).resolve().parent;v=sys.argv[1];src=p/f'src_{v}';out=p/f'build_{v}';out.mkdir(exist_ok=True);cmds=json.loads((p/'build_baseline/commands.json').read_text());new=[]
for cmd in cmds:
 if '-c'in cmd and not any(x.endswith(('ExplicitInertialSolver.cpp','harness.cpp'))for x in cmd):continue
 c=[x.replace(str(p/'src_baseline'),str(src)).replace(str(p/'build_baseline/ExplicitInertialSolver.o'),str(out/'ExplicitInertialSolver.o')).replace(str(p/'build_baseline/harness.o'),str(out/'harness.o')).replace(str(p/'build_baseline/review'),str(out/'review'))for x in cmd];subprocess.run(c,check=True);new.append(c)
(out/'commands.json').write_text(json.dumps(new,indent=2))
