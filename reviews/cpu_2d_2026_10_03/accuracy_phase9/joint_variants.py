from pathlib import Path
import shutil
p=Path(__file__).resolve().parent;f='src/engine/2d/solver/ExplicitInertialSolver.cpp'
for v,src in [('shore_joint','shore_positive'),('connected_joint','connected_positive')]:
 shutil.copytree(p/f'src_{src}',p/f'src_{v}',dirs_exist_ok=True);s=(p/f'src_{v}'/f).read_text().replace('        phi[3] *= positive_scale;','        phi[3] *= positive_scale;\n        phi[0] *= positive_scale;');(p/f'src_{v}'/f).write_text(s)
s=(p/'screen.py').read_text().replace("['baseline','centered','shore_centered','connected_centered','shore_positive','connected_positive','positive_oldsource']","['shore_joint','connected_joint']").replace("'screen.json'","'joint_results.json'");(p/'screen_joint.py').write_text(s)
