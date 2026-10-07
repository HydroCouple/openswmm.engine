from pathlib import Path
import json,subprocess
p=Path(__file__).resolve().parent;s=(p/'harness.cpp').read_text();s=s.replace('auto exact=[&]', '''const double phase=std::getenv("REVIEW_PHASE")?std::atof(std::getenv("REVIEW_PHASE")):0;
 const double displacement=std::getenv("REVIEW_DISPLACEMENT")?std::atof(std::getenv("REVIEW_DISPLACEMENT")):.5;
 const double radial_amplitude=std::getenv("REVIEW_RADIAL_AMPLITUDE")?std::atof(std::getenv("REVIEW_RADIAL_AMPLITUDE")):(1-.8*.8)/(1+.8*.8);
 auto exact=[&]''')
s=s.replace('x-.5*std::cos(omega*t)','x-displacement*std::cos(omega*t+phase)').replace('y-.5*std::sin(omega*t)','y-displacement*std::sin(omega*t+phase)').replace('-.5*omega*std::sin(omega*t)','-displacement*omega*std::sin(omega*t+phase)').replace('.5*omega*std::cos(omega*t)','displacement*omega*std::cos(omega*t+phase)');s=s.replace('double A=(1-.8*.8)/(1+.8*.8),den','double A=radial_amplitude,den');f=p/'stress.cpp';f.write_text(s)
cmds=json.loads((p/'build_baseline/commands.json').read_text());saved=[]
for v in ['baseline','selected']:
 for c in cmds[-2:]:
  c=[str(f)if x==str(p/'harness.cpp')else str(p/f'build_{v}/stress.o')if x==str(p/'build_baseline/harness.o')else str(p/f'build_{v}/ExplicitInertialSolver.o')if x==str(p/'build_baseline/ExplicitInertialSolver.o')else str(p/f'build_{v}/stress')if x==str(p/'build_baseline/review')else x.replace(str(p/'src_baseline'),str(p/f'src_{v}'))for x in c];subprocess.run(c,check=True);saved.append(c)
(p/'stress_commands.json').write_text(json.dumps(saved,indent=2))
