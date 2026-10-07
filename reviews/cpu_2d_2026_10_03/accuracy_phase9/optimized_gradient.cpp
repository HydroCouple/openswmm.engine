void ExplicitInertialSolver::computeLimitedGradientsSwe() {
    const auto depth_offset = state_->depth.size();
    const auto& ed = edges_;
    const int na = static_cast<int>(active_cells_.size());
    const double dry = opts_->dry_depth;
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int k = 0; k < na; ++k) {
        const int i = active_cells_[static_cast<std::size_t>(k)];
        gex_[depth_offset + i] = gey_[depth_offset + i] = gex_[i] = gey_[i] = gux_[i] = guy_[i] = gvx_[i] = gvy_[i] = 0.0;
        const double hi = state_->depth[i];
        if (hi <= 10.0 * dry) continue;
        const double ei = state_->head[i];
        const double ui = qcx_[i] / hi, vi = qcy_[i] / hi;
        const double wi[4] = {ei, ui, vi, hi};
        double gx[4] = {}, gy[4] = {};
        double wmin[4] = {ei, ui, vi, hi}, wmax[4] = {ei, ui, vi, hi};
        const int begin = ed.cell_ptr[i], end = ed.cell_ptr[i + 1];
        unsigned connected = 0;
        int nfaces = 0;
        bool shore = false;
        for (int p = begin; p < end; ++p) {
            const int e = ed.cell_edge[p];
            const int j = (ed.cL[e] == i) ? ed.cR[e] : ed.cL[e];
            const double hj = state_->depth[j];
            if (hj <= dry || !cell_active_[j]) { shore = true; continue; }
            const double ej = state_->head[j];
            const double sill = std::max(ei - hi, ej - hj);
            // A wet neighbour behind a dry sill is not a sample of this
            // cell's connected water surface or velocity field.
            if (ei - sill <= dry || ej - sill <= dry) { shore = true; continue; }
            connected |= 1u << (p - begin);
            const double uj = qcx_[j] / hj, vj = qcy_[j] / hj;
            const double sgn = static_cast<double>(ed.cell_sign[p]);
            const double nx = sgn * ed.nx[e] * ed.xi[e], ny = sgn * ed.ny[e] * ed.xi[e];
            const double w[4] = {0.5 * (ei + ej), 0.5 * (ui + uj), 0.5 * (vi + vj), 0.5 * (hi + hj)};
            const double wj[4] = {ej, uj, vj, hj};
            for (int m = 0; m < 4; ++m) {
                gx[m] += w[m] * nx; gy[m] += w[m] * ny;
                wmin[m] = std::min(wmin[m], wj[m]); wmax[m] = std::max(wmax[m], wj[m]);
            }
            ++nfaces;
        }
        if (nfaces == 0) continue;
        const int nvc = mesh_->cell_vertex_count(i);
        double phi[4] = {1.0, 1.0, 1.0, 1.0};
        if (shore) {
            if (nfaces < 2) continue;
            // Fit eta and depth to connected wet neighbours. Inverse squared
            // distance weighting makes the geometry test scale independent.
            // Keep this extra work out of the fully wet interior path.
            double xx = 0.0, xy = 0.0, yy = 0.0;
            double bx[4] = {}, by[4] = {};
            for (int p = begin; p < end; ++p) {
                if (!(connected & (1u << (p - begin)))) continue;
                const int e = ed.cell_edge[p];
                const int j = (ed.cL[e] == i) ? ed.cR[e] : ed.cL[e];
                const double dx = mesh_->tri_cx[j] - mesh_->tri_cx[i];
                const double dy = mesh_->tri_cy[j] - mesh_->tri_cy[i];
                const double wt = 1.0 / (dx * dx + dy * dy);
                xx += wt * dx * dx; xy += wt * dx * dy; yy += wt * dy * dy;
                const double wj[4] = {state_->head[j], 0.0, 0.0, state_->depth[j]};
                for (int m : {0, 3}) {
                    bx[m] += wt * dx * (wj[m] - wi[m]);
                    by[m] += wt * dy * (wj[m] - wi[m]);
                }
            }
            const double det = xx * yy - xy * xy;
            // Nearly opposite wet neighbours do not reliably constrain a
            // transverse slope. Require a Gram-matrix condition number <98.
            if (det <= 1e-2 * (xx + yy) * (xx + yy)) continue;
            for (int m : {0, 3}) {
                gx[m] = (yy * bx[m] - xy * by[m]) / det;
                gy[m] = (xx * by[m] - xy * bx[m]) / det;
            }
            // A drying cell must export its own velocity. Extrapolating it
            // from the remaining wet neighbours can remove water faster
            // than momentum and accelerate the residual film.
            gx[1] = gy[1] = gx[2] = gy[2] = 0.0;
            phi[1] = phi[2] = 0.0;
        } else {
            // Physical boundary faces have no CSR entry: zero-gradient
            // contribution w_i*n*length completes the Green-Gauss sum.
            if (nfaces < nvc) {
                for (int kk = 0; kk < nvc; ++kk) {
                    if (mesh_->cell_neighbour(i, kk) >= 0) continue;
                    const int slot = MeshData::slot(i, kk);
                    const double nx = mesh_->edge_nx[slot] * mesh_->edge_length[slot];
                    const double ny = mesh_->edge_ny[slot] * mesh_->edge_length[slot];
                    for (int m = 0; m < 4; ++m) { gx[m] += wi[m] * nx; gy[m] += wi[m] * ny; }
                }
            }
            const double inv_a = 1.0 / mesh_->tri_area[i];
            for (int m = 0; m < 4; ++m) { gx[m] *= inv_a; gy[m] *= inv_a; }
        }
        for (int p = begin; p < end; ++p) {
            if (!(connected & (1u << (p - begin)))) continue;
            const double ax = ed.cell_arm_x[p], ay = ed.cell_arm_y[p];
            for (int m = 0; m < 4; ++m) {
                const double wf = wi[m] + gx[m] * ax + gy[m] * ay;
                phi[m] = std::min(phi[m], swe::bjLimiter(wi[m], wf, wmin[m], wmax[m]));
            }
        }
        // Connected interior faces are already bounded by positive wet
        // depths. At shore/boundary cells also constrain the omitted faces,
        // so clipping a negative face value cannot create a different depth
        // polynomial. Scale eta with h to preserve a flat reconstructed bed.
        if (nfaces < nvc) {
            double positive_scale = 1.0;
            for (int kk = 0; kk < nvc; ++kk) {
                const int va = mesh_->cell_vertex(i, kk);
                const int vb = mesh_->cell_vertex(i, (kk + 1) % nvc);
                const double ax = .5 * (mesh_->vx[va] + mesh_->vx[vb]) - mesh_->tri_cx[i];
                const double ay = .5 * (mesh_->vy[va] + mesh_->vy[vb]) - mesh_->tri_cy[i];
                const double dh = phi[3] * (gx[3] * ax + gy[3] * ay);
                if (dh < -hi) positive_scale = std::min(positive_scale, -hi / dh);
            }
            phi[3] *= positive_scale;
            phi[0] *= positive_scale;
        }
        gex_[depth_offset + i] = phi[3] * gx[3]; gey_[depth_offset + i] = phi[3] * gy[3];
        gex_[i] = phi[0] * gx[0]; gey_[i] = phi[0] * gy[0];
        gux_[i] = phi[1] * gx[1]; guy_[i] = phi[1] * gy[1];
        gvx_[i] = phi[2] * gx[2]; gvy_[i] = phi[2] * gy[2];
    }
}

