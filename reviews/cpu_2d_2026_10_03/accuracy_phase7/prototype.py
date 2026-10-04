from pathlib import Path
import shutil,json,subprocess
p=Path(__file__).resolve().parent;dest=p/'src_recon';shutil.copytree(p/'src_baseline',dest,dirs_exist_ok=True)
f=dest/'src/engine/2d/solver/ExplicitInertialSolver.cpp';s=f.read_text();h=f.with_suffix('.hpp');hs=h.read_text()
hs=hs.replace('std::vector<double>  gex_', 'std::vector<double> ghx_, ghy_;\n    std::vector<double>  gex_')
assert 'ghx_'in hs
s=s.replace('gex_.assign(un, 0.0); gey_.assign(un, 0.0);','gex_.assign(un, 0.0); gey_.assign(un, 0.0);\n            ghx_.assign(un, 0.0); ghy_.assign(un, 0.0);')
s=s.replace('gex_.clear(); gey_.clear();','ghx_.clear(); ghy_.clear(); gex_.clear(); gey_.clear();')
s=s.replace('wet = swe::faceFluxRecon(etaLf, uLf, vLf, za, ha,','''const double hLf = std::max(0.0, ha + ghx_[a] * axa + ghy_[a] * aya);
            const double hRf = std::max(0.0, hb + ghx_[b] * axb + ghy_[b] * ayb);
            wet = swe::faceFluxRecon(etaLf, uLf, vLf, za, ha,''')
s=s.replace('ed.nx[e], ed.ny[e], dry, F, cLx, cLy, cRx, cRy);','ed.nx[e], ed.ny[e], dry, F, cLx, cLy, cRx, cRy, etaLf - hLf, etaRf - hRf);',1)
a=s.index('void ExplicitInertialSolver::computeLimitedGradientsSwe()');b=s.index('// SSP-RK2',a);part=s[a:b]
part=part.replace('gex_[i] = gey_[i] =','ghx_[i] = ghy_[i] = gex_[i] = gey_[i] =')
part=part.replace('gx[3] = {0, 0, 0}, gy[3] = {0, 0, 0}','gx[4] = {0, 0, 0, 0}, gy[4] = {0, 0, 0, 0}').replace('wmin[3] = {ei, ui, vi}, wmax[3] = {ei, ui, vi}','wmin[4] = {ei, ui, vi, hi}, wmax[4] = {ei, ui, vi, hi}').replace('w[3] = {0.5 * (ei + ej), 0.5 * (ui + uj), 0.5 * (vi + vj)}','w[4] = {0.5 * (ei + ej), 0.5 * (ui + uj), 0.5 * (vi + vj), 0.5 * (hi + hj)}').replace('wj[3] = {ej, uj, vj}','wj[4] = {ej, uj, vj, hj}').replace('m < 3','m < 4').replace('gx[2] += vi * nx; gy[2] += vi * ny;','gx[2] += vi * nx; gy[2] += vi * ny;\n                gx[3] += hi * nx; gy[3] += hi * ny;').replace('wi[3] = {ei, ui, vi}','wi[4] = {ei, ui, vi, hi}').replace('phi[3] = {1.0, 1.0, 1.0}','phi[4] = {1.0, 1.0, 1.0, 1.0}').replace('gex_[i] = phi[0]', 'ghx_[i] = phi[3] * gx[3]; ghy_[i] = phi[3] * gy[3];\n        gex_[i] = phi[0]')
s=s[:a]+part+s[b:];f.write_text(s);h.write_text(hs)
f=dest/'src/engine/2d/solver/SweKernels.hpp';s=f.read_text();a=s.index('OPENSWMM_KERNEL_FN bool faceFluxRecon');b=s.index('/// Barth',a);part=s[a:b].replace('double& corrR_x, double& corrR_y) noexcept {','double& corrR_x, double& corrR_y,\n                                      double zLf, double zRf) noexcept {').replace('const double zf = (zL > zR) ? zL : zR;','const double zf = (zLf > zRf) ? zLf : zRf;');s=s[:a]+part+s[b:];f.write_text(s)
cmds=json.loads((p/'build_baseline/commands.json').read_text());out=p/'build_recon';out.mkdir(exist_ok=True);new=[]
for cmd in cmds:
 if '-c'in cmd and not any(x.endswith(('ExplicitInertialSolver.cpp','harness.cpp'))for x in cmd):continue
 c=[x.replace(str(p/'src_baseline'),str(dest)).replace(str(p/'build_baseline/ExplicitInertialSolver.o'),str(out/'ExplicitInertialSolver.o')).replace(str(p/'build_baseline/harness.o'),str(out/'harness.o')).replace(str(p/'build_baseline/review'),str(out/'review'))for x in cmd];subprocess.run(c,check=True);new.append(c)
(out/'commands.json').write_text(json.dumps(new,indent=2));print('Depth/bed reconstruction prototype built')
