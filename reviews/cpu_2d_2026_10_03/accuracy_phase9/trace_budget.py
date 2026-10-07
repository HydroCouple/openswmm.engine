from pathlib import Path
import shutil,subprocess,os,json
p=Path(__file__).resolve().parent;v='trace';f='src/engine/2d/solver/ExplicitInertialSolver.cpp';shutil.copytree(p/'src_connected_joint',p/f'src_{v}',dirs_exist_ok=True);s=(p/f'src_{v}'/f).read_text();a=s.index('void ExplicitInertialSolver::fireFacesSwe');b=s.index('void ExplicitInertialSolver::fireFacesDiffusive',a);g=s[a:b]
g=g.replace('    const auto& ed = edges_;','    const auto& ed = edges_;\n    static int pass = 0; ++pass;',1)
g=g.replace('        // Species ride the mass flux', '''        if (a == 596 || b == 596) {
            const int i = 596;
            const double sign = a == i ? 1.0 : -1.0;
            const double cx = a == i ? cLx : cRx, cy = a == i ? cLy : cRy;
            std::fprintf(stderr,"FACE %d %d %d %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g\\n",pass,e,a==i?b:a,dt_f,state_->depth[i],qcx_[i],qcy_[i],-sign*fh*xdt,-sign*fmx*xdt,-sign*fmy*xdt,cx*xdt,cy*xdt,gex_[i],gey_[i],mesh_->tri_area[i]);
        }
        // Species ride the mass flux''')
s=s[:a]+g+s[b:];(p/f'src_{v}'/f).write_text(s)
subprocess.run(['python3',str(p/'build_variant.py'),v],check=True)
r=subprocess.run([str(p/f'build_{v}/review'),'planar','32','1','2','1','2','1'],env={**os.environ,'REVIEW_SKEW':'.15'},capture_output=True,text=True,check=True);(p/'budget_trace.log').write_text(r.stderr);(p/'budget_trace_result.json').write_text(r.stdout)
