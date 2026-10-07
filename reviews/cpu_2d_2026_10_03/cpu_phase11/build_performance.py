from pathlib import Path
import json,subprocess,sys
p=Path(__file__).resolve().parent;cmds=json.loads((p/'build_baseline/commands.json').read_text());saved=[]
for v in sys.argv[1:]:
 obj=str(p/f'build_{v}/performance.o');old=str(p/'build_baseline/harness.o');cmd=[str(p/'performance.cpp')if x==str(p/'harness.cpp')else obj if x==old else x.replace(str(p/'src_baseline'),str(p/f'src_{v}'))for x in cmds[-2]];subprocess.run(cmd,check=True)
 link=[obj if x==old else str(p/f'build_{v}/ExplicitInertialSolver.o')if x==str(p/'build_baseline/ExplicitInertialSolver.o')else str(p/f'build_{v}/performance')if x==str(p/'build_baseline/review')else x for x in cmds[-1]];subprocess.run(link,check=True);saved.extend([cmd,link])
(p/'performance_commands.json').write_text(json.dumps(saved,indent=2))
