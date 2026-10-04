from pathlib import Path
p=Path(__file__).resolve().parent;root=p/'src_selected/src/engine/2d/solver';f=root/'ExplicitInertialSolver.cpp';s=f.read_text()
s=s.replace('gex_.assign(un, 0.0); gey_.assign(un, 0.0);\n            ghx_.assign(un, 0.0); ghy_.assign(un, 0.0);','gex_.assign(2 * un, 0.0); gey_.assign(2 * un, 0.0);')
s=s.replace('ghx_.clear(); ghy_.clear(); ', '')
s=s.replace('ghx_[a]', 'gex_[depth_offset + a]').replace('ghy_[a]', 'gey_[depth_offset + a]').replace('ghx_[b]', 'gex_[depth_offset + b]').replace('ghy_[b]', 'gey_[depth_offset + b]').replace('ghx_[i]', 'gex_[depth_offset + i]').replace('ghy_[i]', 'gey_[depth_offset + i]')
s=s.replace('    if (second_order_) computeLimitedGradientsSwe();','    const auto depth_offset = state_->depth.size();\n    if (second_order_) computeLimitedGradientsSwe();',1)
s=s.replace('void ExplicitInertialSolver::computeLimitedGradientsSwe() {','void ExplicitInertialSolver::computeLimitedGradientsSwe() {\n    const auto depth_offset = state_->depth.size();',1)
f.write_text(s);f=root/'ExplicitInertialSolver.hpp';s=f.read_text();s=s.replace('    /// Limited depth gradients supply the second-order face bed (eta - h).\n    std::vector<double> ghx_, ghy_;\n', '    /// gex_/gey_: eta gradients in [0, n), depth gradients in [n, 2*n).\n    /// The second block supplies the face bed (eta - h) only for order 2.\n');f.write_text(s)
