/**
 * @file HypreAmgPreconditioner.cpp
 * @brief hypre BoomerAMG preconditioner implementation (built only when
 *        OPENSWMM_WITH_HYPRE is ON).
 *
 * @see HypreAmgPreconditioner.hpp
 * @ingroup engine_2d
 */

#include "HypreAmgPreconditioner.hpp"
#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"

#include <HYPRE.h>
#include <HYPRE_IJ_mv.h>
#include <HYPRE_parcsr_ls.h>
#include <HYPRE_utilities.h>

#include <numeric>
#include <vector>

namespace openswmm::twoD {

namespace {
// Sequential hypre (overlay build, HYPRE_WITH_MPI=OFF → HYPRE_SEQUENTIAL):
// MPI_Comm is just HYPRE_Int and the communicator is ignored by the mpistubs
// (hypre_MPI_COMM_WORLD == 0). Passing 0 avoids any dependency on a real MPI
// implementation or hypre's internal headers.
inline MPI_Comm seqComm() { return static_cast<MPI_Comm>(0); }
} // namespace

void HypreAmgPreconditioner::initialize(const MeshData& mesh) {
    finalize();
    jac_.buildSparsity(mesh);
    n_ = jac_.rows();
    if (n_ <= 0) return;

    rows_.resize(static_cast<std::size_t>(n_));
    std::iota(rows_.begin(), rows_.end(), 0);

    const int* rp = jac_.rowPtr();
    ncols_.resize(static_cast<std::size_t>(n_));
    for (int i = 0; i < n_; ++i) ncols_[i] = rp[i + 1] - rp[i];

    const HYPRE_BigInt lo = 0, hi = static_cast<HYPRE_BigInt>(n_) - 1;

    HYPRE_IJMatrix A;
    HYPRE_IJMatrixCreate(seqComm(), lo, hi, lo, hi, &A);
    HYPRE_IJMatrixSetObjectType(A, HYPRE_PARCSR);
    HYPRE_IJMatrixInitialize(A);
    A_ = A;

    HYPRE_IJVector b, x;
    HYPRE_IJVectorCreate(seqComm(), lo, hi, &b);
    HYPRE_IJVectorSetObjectType(b, HYPRE_PARCSR);
    HYPRE_IJVectorInitialize(b);
    b_ = b;
    HYPRE_IJVectorCreate(seqComm(), lo, hi, &x);
    HYPRE_IJVectorSetObjectType(x, HYPRE_PARCSR);
    HYPRE_IJVectorInitialize(x);
    x_ = x;

    // BoomerAMG configured as a single-V-cycle stationary preconditioner. The
    // coarsen/interp/relax choices follow the design doc (HMIS coarsening,
    // extended+i interpolation, hybrid symmetric Gauss-Seidel smoothing), which
    // are robust for the near-symmetric M-matrix of the diffusive-wave operator.
    // Aggressive coarsening on the top 2 levels (PMIS + multipass interpolation)
    // makes each V-cycle and the setup markedly cheaper. The diffusive-wave
    // operator is well-conditioned enough that GMRES still converges in a handful
    // of iterations (large headroom under the restart dim), so the extra-iteration
    // cost is far outweighed — measured ~1.29x faster on a 100k-cell 24 h run with
    // continuity unchanged.
    HYPRE_Solver amg;
    HYPRE_BoomerAMGCreate(&amg);
    HYPRE_BoomerAMGSetPrintLevel(amg, 0);
    HYPRE_BoomerAMGSetCoarsenType(amg, 8);        // PMIS
    HYPRE_BoomerAMGSetInterpType(amg, 6);         // extended+i
    HYPRE_BoomerAMGSetRelaxType(amg, 6);          // hybrid symmetric Gauss-Seidel
    HYPRE_BoomerAMGSetStrongThreshold(amg, 0.25);
    HYPRE_BoomerAMGSetMaxLevels(amg, 25);
    HYPRE_BoomerAMGSetTol(amg, 0.0);              // fixed-iteration (preconditioner)
    HYPRE_BoomerAMGSetMaxIter(amg, 1);            // one V-cycle per apply
    HYPRE_BoomerAMGSetAggNumLevels(amg, 2);       // aggressive coarsening: cheaper V-cycle
    HYPRE_BoomerAMGSetAggInterpType(amg, 4);      // multipass interp for aggressive levels
    amg_ = amg;
}

void HypreAmgPreconditioner::setup(const MeshData& mesh,
                                    const SurfaceStateData& state, double gamma,
                                    bool recompute, const double* deta_dv) {
    if (n_ <= 0) return;

    // Lagged preconditioner: when CVODE says the saved Jacobian is still current
    // (jok == SUNTRUE → recompute == false) and a hierarchy already exists,
    // reuse the prior matrix + multigrid hierarchy verbatim. This skips the
    // O(n) Jacobian assembly AND the dominant HYPRE_BoomerAMGSetup. GMRES still
    // uses the true matrix-free operator, so a slightly stale preconditioner
    // only costs a few extra Krylov iterations, never correctness.
    if (!recompute && hierarchy_built_) return;

    // Refresh M = I − γ·J and overwrite the matrix values (structure is fixed).
    jac_.assemble(mesh, state, gamma, 1.0e-9, deta_dv);

    auto A = static_cast<HYPRE_IJMatrix>(A_);
    HYPRE_IJMatrixInitialize(A);
    HYPRE_IJMatrixSetValues(A, n_, ncols_.data(),
                            reinterpret_cast<const HYPRE_BigInt*>(rows_.data()),
                            reinterpret_cast<const HYPRE_BigInt*>(jac_.colIdx()),
                            jac_.values());
    HYPRE_IJMatrixAssemble(A);

    HYPRE_ParCSRMatrix parA;
    HYPRE_IJMatrixGetObject(A, reinterpret_cast<void**>(&parA));

    auto b = static_cast<HYPRE_IJVector>(b_);
    auto x = static_cast<HYPRE_IJVector>(x_);
    HYPRE_ParVector parb, parx;
    HYPRE_IJVectorGetObject(b, reinterpret_cast<void**>(&parb));
    HYPRE_IJVectorGetObject(x, reinterpret_cast<void**>(&parx));

    // Rebuild the multigrid hierarchy on the fresh matrix. CVODE's psetup
    // policy controls how often this runs (lagged across Newton iterations).
    HYPRE_BoomerAMGSetup(static_cast<HYPRE_Solver>(amg_), parA, parb, parx);
    hierarchy_built_ = true;
}

void HypreAmgPreconditioner::solve(const double* r, double* z, int n) {
    if (n_ <= 0 || n != n_) return;

    auto b = static_cast<HYPRE_IJVector>(b_);
    auto x = static_cast<HYPRE_IJVector>(x_);

    HYPRE_IJVectorInitialize(b);
    HYPRE_IJVectorSetValues(b, n_,
                            reinterpret_cast<const HYPRE_BigInt*>(rows_.data()), r);
    HYPRE_IJVectorAssemble(b);

    // Zero initial guess for the V-cycle.
    std::vector<double> z0(static_cast<std::size_t>(n_), 0.0);
    HYPRE_IJVectorInitialize(x);
    HYPRE_IJVectorSetValues(x, n_,
                            reinterpret_cast<const HYPRE_BigInt*>(rows_.data()),
                            z0.data());
    HYPRE_IJVectorAssemble(x);

    HYPRE_ParCSRMatrix parA;
    HYPRE_IJMatrixGetObject(static_cast<HYPRE_IJMatrix>(A_),
                            reinterpret_cast<void**>(&parA));
    HYPRE_ParVector parb, parx;
    HYPRE_IJVectorGetObject(b, reinterpret_cast<void**>(&parb));
    HYPRE_IJVectorGetObject(x, reinterpret_cast<void**>(&parx));

    HYPRE_BoomerAMGSolve(static_cast<HYPRE_Solver>(amg_), parA, parb, parx);

    HYPRE_IJVectorGetValues(x, n_,
                            reinterpret_cast<const HYPRE_BigInt*>(rows_.data()), z);
}

void HypreAmgPreconditioner::finalize() {
    if (amg_) { HYPRE_BoomerAMGDestroy(static_cast<HYPRE_Solver>(amg_)); amg_ = nullptr; }
    if (A_)   { HYPRE_IJMatrixDestroy(static_cast<HYPRE_IJMatrix>(A_));  A_ = nullptr; }
    if (b_)   { HYPRE_IJVectorDestroy(static_cast<HYPRE_IJVector>(b_));  b_ = nullptr; }
    if (x_)   { HYPRE_IJVectorDestroy(static_cast<HYPRE_IJVector>(x_));  x_ = nullptr; }
    n_ = 0;
    hierarchy_built_ = false;
}

HypreAmgPreconditioner::~HypreAmgPreconditioner() { finalize(); }

} // namespace openswmm::twoD
