#include "RunoffElasticity.hpp"
#include "../core/SimulationContext.hpp"

#include <algorithm>
#include <cmath>

namespace openswmm::uncertainty {

namespace {
// The ctx.subcatches arrays RunoffSolver::execute() writes. Snapshotted around
// each member run so the deterministic step sees exactly what it would have.
struct CtxSnapshot {
    std::vector<double> rainfall, runoff, evap_loss, infil_loss;
    std::vector<double> stat_evap_vol, stat_infil_vol, stat_imperv_vol, stat_perv_vol;
    std::vector<double> lid_return_to_perv_cfs, lid_return_applied_cfs;
    void save(const SubcatchData& sc) {
        rainfall = sc.rainfall; runoff = sc.runoff; evap_loss = sc.evap_loss;
        infil_loss = sc.infil_loss; stat_evap_vol = sc.stat_evap_vol;
        stat_infil_vol = sc.stat_infil_vol; stat_imperv_vol = sc.stat_imperv_vol;
        stat_perv_vol = sc.stat_perv_vol; lid_return_to_perv_cfs = sc.lid_return_to_perv_cfs;
        lid_return_applied_cfs = sc.lid_return_applied_cfs;  // written by execute() since swmm6_rel 2026-11
    }
    void restore(SubcatchData& sc) const {
        sc.rainfall = rainfall; sc.runoff = runoff; sc.evap_loss = evap_loss;
        sc.infil_loss = infil_loss; sc.stat_evap_vol = stat_evap_vol;
        sc.stat_infil_vol = stat_infil_vol; sc.stat_imperv_vol = stat_imperv_vol;
        sc.stat_perv_vol = stat_perv_vol; sc.lid_return_to_perv_cfs = lid_return_to_perv_cfs;
        sc.lid_return_applied_cfs = lid_return_applied_cfs;
    }
};
}  // namespace

void RunoffElasticityProbe::step(runoff::RunoffSolver& solver, SimulationContext& ctx,
                                 double dt, double evap_rate, double infil_factor,
                                 double recovery_factor, int month,
                                 const std::vector<int>* source_owners) {
    const int n = ctx.subcatches.count();
    if (n <= 0) return;
    const auto un = static_cast<std::size_t>(n);

    const runoff::RunoffSolver::State det = solver.saveState();
    if (!seeded_) {
        minus_ = det;
        plus_  = det;
        q_minus_.assign(un, 0.0);
        q_plus_.assign(un, 0.0);
        elasticity_.assign(un, 1.0);
        seeded_ = true;
    }

    CtxSnapshot snap;
    snap.save(ctx.subcatches);

    auto run_member = [&](runoff::RunoffSolver::State& st, double scale,
                          std::vector<double>& q_out) {
        solver.restoreState(st);
        // Same arguments as the engine's deterministic call (no boundary,
        // source forcing or spatial fractions; the same source-owner map),
        // then the rain scale.
        solver.execute(ctx, dt, evap_rate, infil_factor, recovery_factor, month,
                       nullptr, nullptr, nullptr, source_owners, scale);
        for (std::size_t i = 0; i < un; ++i) q_out[i] = ctx.subcatches.runoff[i];
        st = solver.saveState();
        snap.restore(ctx.subcatches);   // the consumed lid_return and the outputs
    };
    run_member(minus_, 1.0 - delta, q_minus_);
    run_member(plus_,  1.0 + delta, q_plus_);

    solver.restoreState(det);
    snap.restore(ctx.subcatches);

    const double denom = std::log((1.0 + delta) / (1.0 - delta));
    for (std::size_t i = 0; i < un; ++i) {
        const double qm = q_minus_[i], qp = q_plus_[i];
        double e = 1.0;
        if (qm > 0.0 && qp > 0.0 && denom > 0.0)
            e = std::log(qp / qm) / denom;
        elasticity_[i] = std::clamp(e, 0.0, max_elasticity);
    }
    ++n_steps_;
}

} // namespace openswmm::uncertainty
