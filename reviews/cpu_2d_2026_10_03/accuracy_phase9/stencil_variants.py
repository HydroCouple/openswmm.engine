from pathlib import Path
import shutil
p=Path(__file__).resolve().parent;f='src/engine/2d/solver/ExplicitInertialSolver.cpp'
for v,src,cut in [('shore_conditioned','shore_joint','1e-2'),('connected_conditioned','connected_joint','1e-2'),('connected_first_velocity','connected_joint','1e-2')]:
 shutil.copytree(p/f'src_{src}',p/f'src_{v}',dirs_exist_ok=True);s=(p/f'src_{v}'/f).read_text().replace('det <= 1e-12 *','det <= '+cut+' *')
 if v=='connected_first_velocity':s=s.replace('        phi[3] *= positive_scale;','        if (shore) phi[1] = phi[2] = 0.0;\n        phi[3] *= positive_scale;')
 (p/f'src_{v}'/f).write_text(s)
s=(p/'screen.py').read_text().replace("['baseline','centered','shore_centered','connected_centered','shore_positive','connected_positive','positive_oldsource']","['shore_conditioned','connected_conditioned','connected_first_velocity']").replace("'screen.json'","'stencil_results.json'");(p/'screen_stencil.py').write_text(s)
