from pathlib import Path
import json,subprocess
p=Path(__file__).resolve().parent;s=(p/'harness.cpp').read_text();a=s.index(' FILE* history=');b=s.index(' auto start=std::chrono',a);s=s[:a]+s[b:];s=s.replace('diagnostic(t);','').replace(' if(history)std::fclose(history);','');f=p/'performance.cpp';f.write_text(s)
cmds=json.loads((p/'build_baseline/commands.json').read_text());saved=[]
for v in ['baseline','selected']:
 obj=str(p/f'build_{v}/performance.o');old=str(p/'build_baseline/harness.o');cmd=[str(f)if x==str(p/'harness.cpp')else obj if x==old else x.replace(str(p/'src_baseline'),str(p/f'src_{v}'))for x in cmds[-2]];subprocess.run(cmd,check=True)
 link=[obj if x==old else str(p/f'build_{v}/ExplicitInertialSolver.o')if x==str(p/'build_baseline/ExplicitInertialSolver.o')else str(p/f'build_{v}/performance')if x==str(p/'build_baseline/review')else x for x in cmds[-1]];subprocess.run(link,check=True);saved.extend([cmd,link])
(p/'performance_commands.json').write_text(json.dumps(saved,indent=2))
