from pathlib import Path
import difflib,shutil
R=Path(__file__).resolve().parent;p=Path('src/engine/hydraulics/fv/ExplicitFvSolver.cpp');s=(R/'source_baseline'/p).read_text();anchor='    // Re-solve the LIVE incident faces at a trial head; held faces contribute'
a=s.index(anchor)
new='''    // Trial heads change only the ghost. Keep ordinary faces' bed and
    // reconstructed cell state private to this solve and its executing worker.
    // Reset readiness on every invocation: no state survives an RK stage,
    // re-tier, rollback, or a different solver using this thread.
    struct TrialFace {
        bool ready = false;
        double zstar = 0.0;
        double u_interior = 0.0;
        k::FaceState cell;
    };
    static thread_local std::vector<TrialFace> trial_faces;
    trial_faces.resize(static_cast<std::size_t>(e - b));
    for (auto& entry : trial_faces) entry.ready = false;
    auto trialFlux = [&](int p, int f) {
        const auto uf = static_cast<std::size_t>(f);
        const int cl = mesh_->face_cl[uf], cr = mesh_->face_cr[uf];
        if (mesh_->face_node[uf] != n || (cl >= 0) == (cr >= 0) ||
            mesh_->face_gate[uf] != 0 || mesh_->face_culvert[uf] >= 0) {
            computeFaceFlux(f, true);
            return;
        }
        auto& entry = trial_faces[static_cast<std::size_t>(p - b)];
        const bool cell_left = cl >= 0;
        if (!entry.ready) {
            k::FaceState probe;
            double zl = 0.0, zr = 0.0, i1 = 0.0;
            faceSide(f, cl, n, 0.0, mesh_->face_dir_l[uf], 0.0, probe, i1, zl, true);
            faceSide(f, cr, n, 0.0, mesh_->face_dir_r[uf], 0.0, probe, i1, zr, true);
            entry.zstar = std::max(zl, zr);
            const int cell = cell_left ? cl : cr;
            const int dir = cell_left ? mesh_->face_dir_l[uf] : mesh_->face_dir_r[uf];
            entry.u_interior = static_cast<double>(dir) *
                cell_u_[static_cast<std::size_t>(cell)];
            // faceSide can leave default fields untouched (e.g. press when
            // mixed-wave handling is off); start as a fresh FaceState does.
            entry.cell = k::FaceState{};
            double unused = 0.0;
            faceSide(f, cell, n, entry.zstar, dir, entry.u_interior,
                     entry.cell, i1, unused, false);
            entry.ready = true;
        }
        k::FaceState ghost;
        double i1 = 0.0, unused = 0.0;
        faceSide(f, cell_left ? cr : cl, n, entry.zstar,
                 cell_left ? mesh_->face_dir_r[uf] : mesh_->face_dir_l[uf],
                 entry.u_interior, ghost, i1, unused, false);
        f_mass_[uf] = cell_left ? k::riemannMassFlux(entry.cell, ghost)
                               : k::riemannMassFlux(ghost, entry.cell);
    };

'''
s=s[:a]+new+s[a:];old='if (la || ra) { perf::count(perf::n_fv_alg_flux); computeFaceFlux(f, true); }';assert s.count(old)==1;s=s.replace(old,'if (la || ra) { perf::count(perf::n_fv_alg_flux); trialFlux(p, f); }');(R/'source_candidate'/p).write_text(s);(R/'candidate.patch').write_text(''.join(difflib.unified_diff((R/'source_baseline'/p).read_text().splitlines(True),s.splitlines(True),fromfile='a/'+str(p),tofile='b/'+str(p))));shutil.copyfile(R/'source_candidate'/p,R.parent/'dw_fv_p0_2026-10-03/source'/p)
