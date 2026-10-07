from pathlib import Path
import shutil
p=Path(__file__).resolve().parent;f='src/engine/2d/solver/ExplicitInertialSolver.cpp';shutil.copytree(p/'src_connected_first_velocity',p/'src_selected',dirs_exist_ok=True);s=(p/'src_selected'/f).read_text();a=s.index('void ExplicitInertialSolver::computeLimitedGradientsSwe()');b=s.index('// SSP-RK2',a);s=s[:a]+(p/'optimized_gradient.cpp').read_text()+s[b:];(p/'src_selected'/f).write_text(s)
