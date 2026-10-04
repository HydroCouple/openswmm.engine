from pathlib import Path
import json,subprocess
p=Path(__file__).resolve().parent;cmds=json.loads((p/'build_baseline/commands.json').read_text());saved=[]
for v in ['baseline','selected']:
 for cmd in cmds[-2:]:
  c=[x.replace(str(p/'src_baseline'),str(p/f'src_{v}')).replace(str(p/'build_baseline/harness.o'),str(p/f'build_{v}/harness.o')).replace(str(p/'build_baseline/ExplicitInertialSolver.o'),str(p/f'build_{v}/ExplicitInertialSolver.o')).replace(str(p/'build_baseline/review'),str(p/f'build_{v}/review'))for x in cmd]
  subprocess.run(c,check=True);saved.append(c)
(p/'harness_commands.json').write_text(json.dumps(saved,indent=2))
