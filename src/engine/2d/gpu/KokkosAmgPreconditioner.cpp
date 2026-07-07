/**
 * @file KokkosAmgPreconditioner.cpp
 * @brief hypre BoomerAMG preconditioner for the Kokkos plugin — host (OpenMP)
 *        and device (CUDA/HIP) paths. Built only when OPENSWMM_WITH_HYPRE is ON.
 *
 * @see KokkosAmgPreconditioner.hpp
 * @ingroup engine_2d_gpu
 */

#include "KokkosAmgPreconditioner.hpp"

#include <HYPRE.h>
#include <HYPRE_IJ_mv.h>
#include <HYPRE_parcsr_ls.h>
#include <HYPRE_utilities.h>

#include <vector>

namespace openswmm::twoD::gpu {

namespace {

// Sequential hypre: MPI_Comm is HYPRE_Int and the communicator is ignored.
inline MPI_Comm seqComm() { return static_cast<MPI_Comm>(0); }

// hypre memory location / execution policy that matches the Kokkos ExecSpace,
// so the IJ matrix/vectors and the BoomerAMG V-cycle stay in the same space as
// the Kokkos N_Vector data (no host↔device copies inside the linear solve).
#if defined(OPENSWMM_GPU_EXECSPACE_CUDA) || defined(OPENSWMM_GPU_EXECSPACE_HIP) \
    || defined(OPENSWMM_GPU_EXECSPACE_SYCL)
constexpr HYPRE_MemoryLocation kMemLoc = HYPRE_MEMORY_DEVICE;
constexpr HYPRE_ExecutionPolicy kExec  = HYPRE_EXEC_DEVICE;
constexpr bool kDevice = true;
#else
constexpr HYPRE_MemoryLocation kMemLoc = HYPRE_MEMORY_HOST;
constexpr HYPRE_ExecutionPolicy kExec  = HYPRE_EXEC_HOST;
constexpr bool kDevice = false;
#endif

} // namespace

void KokkosAmgPreconditioner::initialize(const MeshViews& mesh) {
    finalize();
    n_ = mesh.n_tri;
    if (n_ <= 0) return;

    // ---- Build static CSR sparsity on the host from the topology -----------
    // (diagonal + up to three distinct non-boundary neighbours per row), then
    // mirror the index arrays into the ExecSpace.
    auto h_nb0 = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), mesh.tri_nbr0);
    auto h_nb1 = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), mesh.tri_nbr1);
    auto h_nb2 = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), mesh.tri_nbr2);

    std::vector<int> h_rowptr(static_cast<std::size_t>(n_) + 1, 0);
    std::vector<int> h_col;    h_col.reserve(static_cast<std::size_t>(n_) * 4);
    std::vector<int> h_diag(static_cast<std::size_t>(n_), -1);
    std::vector<int> h_edge(static_cast<std::size_t>(n_) * 3, -1);
    std::vector<int> h_ncols(static_cast<std::size_t>(n_), 0);
    std::vector<int> h_rows(static_cast<std::size_t>(n_), 0);

    for (int i = 0; i < n_; ++i) {
        const int base = static_cast<int>(h_col.size());
        auto find_or_add = [&](int col) -> int {
            for (int p = base; p < static_cast<int>(h_col.size()); ++p)
                if (h_col[p] == col) return p;
            h_col.push_back(col);
            return static_cast<int>(h_col.size()) - 1;
        };
        h_diag[i] = find_or_add(i);
        const int nb[3] = {h_nb0(i), h_nb1(i), h_nb2(i)};
        for (int e = 0; e < 3; ++e)
            if (nb[e] >= 0) h_edge[static_cast<std::size_t>(i) * 3 + e] = find_or_add(nb[e]);
        h_rowptr[i + 1] = static_cast<int>(h_col.size());
        h_ncols[i]      = h_rowptr[i + 1] - h_rowptr[i];
        h_rows[i]       = i;
    }
    const int nnz = static_cast<int>(h_col.size());

    auto toView = [](const std::vector<int>& v, const char* nm) {
        IView dev(Kokkos::view_alloc(std::string(nm)), v.size());
        auto host = Kokkos::create_mirror_view(dev);
        for (std::size_t k = 0; k < v.size(); ++k) host(k) = v[k];
        Kokkos::deep_copy(dev, host);
        return dev;
    };
    row_ptr_  = toView(h_rowptr, "amg_rowptr");
    col_idx_  = toView(h_col,    "amg_colidx");
    diag_pos_ = toView(h_diag,   "amg_diagpos");
    edge_pos_ = toView(h_edge,   "amg_edgepos");
    ncols_    = toView(h_ncols,  "amg_ncols");
    rows_     = toView(h_rows,   "amg_rows");
    values_   = DView(Kokkos::view_alloc(std::string("amg_values")),
                       static_cast<std::size_t>(nnz));

    // ---- hypre objects in the matching memory location ---------------------
    HYPRE_Initialize();
    if (kDevice) {
        HYPRE_SetMemoryLocation(HYPRE_MEMORY_DEVICE);
        HYPRE_SetExecutionPolicy(HYPRE_EXEC_DEVICE);
    }

    const HYPRE_BigInt lo = 0, hi = static_cast<HYPRE_BigInt>(n_) - 1;
    HYPRE_IJMatrix A;
    HYPRE_IJMatrixCreate(seqComm(), lo, hi, lo, hi, &A);
    HYPRE_IJMatrixSetObjectType(A, HYPRE_PARCSR);
    HYPRE_IJMatrixInitialize_v2(A, kMemLoc);
    A_ = A;

    HYPRE_IJVector b, x;
    HYPRE_IJVectorCreate(seqComm(), lo, hi, &b);
    HYPRE_IJVectorSetObjectType(b, HYPRE_PARCSR);
    HYPRE_IJVectorInitialize_v2(b, kMemLoc);
    b_ = b;
    HYPRE_IJVectorCreate(seqComm(), lo, hi, &x);
    HYPRE_IJVectorSetObjectType(x, HYPRE_PARCSR);
    HYPRE_IJVectorInitialize_v2(x, kMemLoc);
    x_ = x;

    // Aggressive coarsening on the top 2 levels (PMIS + multipass interpolation)
    // makes each V-cycle and the setup markedly cheaper; the well-conditioned
    // diffusive-wave operator still converges in a handful of GMRES iterations, so
    // the trade is strongly net-positive (~1.29x on a 100k-cell 24 h run).
    HYPRE_Solver amg;
    HYPRE_BoomerAMGCreate(&amg);
    HYPRE_BoomerAMGSetPrintLevel(amg, 0);
    HYPRE_BoomerAMGSetCoarsenType(amg, 8);        // PMIS
    HYPRE_BoomerAMGSetInterpType(amg, 6);         // extended+i
    // Device backends require a polynomial / ℓ¹-Jacobi smoother (hybrid
    // Gauss-Seidel is host-only in hypre's GPU path); host uses hybrid SGS.
    HYPRE_BoomerAMGSetRelaxType(amg, kDevice ? 18 : 6);  // 18 = ℓ¹-scaled Jacobi
    HYPRE_BoomerAMGSetStrongThreshold(amg, 0.25);
    HYPRE_BoomerAMGSetMaxLevels(amg, 25);
    HYPRE_BoomerAMGSetTol(amg, 0.0);
    HYPRE_BoomerAMGSetMaxIter(amg, 1);            // one V-cycle per apply
    HYPRE_BoomerAMGSetAggNumLevels(amg, 2);       // aggressive coarsening: cheaper V-cycle
    HYPRE_BoomerAMGSetAggInterpType(amg, 4);      // multipass interp for aggressive levels
    amg_ = amg;
}

void KokkosAmgPreconditioner::setup(const MeshViews& mesh, const StateViews& state,
                                     double gamma, bool recompute) {
    if (n_ <= 0) return;

    // Lagged preconditioner: when CVODE says the saved Jacobian is still
    // current (jok == SUNTRUE → recompute == false) and a hierarchy exists,
    // reuse the prior matrix + multigrid hierarchy verbatim — skipping the
    // O(n) assembly AND the dominant HYPRE_BoomerAMGSetup. GMRES still uses
    // the true matrix-free operator, so a slightly stale preconditioner only
    // costs a few extra Krylov iterations, never correctness. Mirrors the
    // serial HypreAmgPreconditioner::setup().
    if (!recompute && hierarchy_built_) return;

    // ---- Assemble M = I − γ·J into values_ in the active ExecSpace ----------
    Kokkos::deep_copy(values_, 0.0);
    auto vals = values_; auto dp = diag_pos_; auto ep = edge_pos_;
    auto nb0 = mesh.tri_nbr0, nb1 = mesh.tri_nbr1, nb2 = mesh.tri_nbr2;
    auto area = mesh.tri_area; auto head = state.head; auto eflux = state.edge_flux;
    Kokkos::parallel_for("amg_assemble", Kokkos::RangePolicy<ExecSpace>(0, n_),
        KOKKOS_LAMBDA(int i) {
            const double inv_area = (area(i) > 1.0e-30) ? 1.0 / area(i) : 0.0;
            const int nb[3] = {nb0(i), nb1(i), nb2(i)};
            double diag = 1.0;
            for (int e = 0; e < 3; ++e) {
                if (nb[e] < 0) continue;
                const double dh = Kokkos::fabs(head(i) - head(nb[e]));
                const double F  = Kokkos::fabs(eflux(i * 3 + e));
                const double T  = F / Kokkos::fmax(dh, 1.0e-9);
                const double m_off = -gamma * T * inv_area;
                diag -= m_off;
                // Each row is one thread; its entry positions are disjoint from
                // every other row, so plain += (which also folds any duplicate-
                // neighbour edge) is race-free.
                vals(ep(i * 3 + e)) += m_off;
            }
            vals(dp(i)) += diag;
        });
    Kokkos::fence();

    auto A = static_cast<HYPRE_IJMatrix>(A_);
    HYPRE_IJMatrixInitialize_v2(A, kMemLoc);
    HYPRE_IJMatrixSetValues(A, n_,
        reinterpret_cast<HYPRE_Int*>(ncols_.data()),
        reinterpret_cast<const HYPRE_BigInt*>(rows_.data()),
        reinterpret_cast<const HYPRE_BigInt*>(col_idx_.data()),
        values_.data());
    HYPRE_IJMatrixAssemble(A);

    HYPRE_ParCSRMatrix parA;
    HYPRE_IJMatrixGetObject(A, reinterpret_cast<void**>(&parA));
    auto b = static_cast<HYPRE_IJVector>(b_);
    auto x = static_cast<HYPRE_IJVector>(x_);
    HYPRE_ParVector parb, parx;
    HYPRE_IJVectorGetObject(b, reinterpret_cast<void**>(&parb));
    HYPRE_IJVectorGetObject(x, reinterpret_cast<void**>(&parx));
    HYPRE_BoomerAMGSetup(static_cast<HYPRE_Solver>(amg_), parA, parb, parx);
    hierarchy_built_ = true;
}

void KokkosAmgPreconditioner::solve(DView r, DView z, double /*gamma*/) {
    if (n_ <= 0) return;
    auto b = static_cast<HYPRE_IJVector>(b_);
    auto x = static_cast<HYPRE_IJVector>(x_);

    // r and z live in the plugin's MemSpace, exactly what hypre expects given
    // the memory location set at initialize().
    HYPRE_IJVectorInitialize_v2(b, kMemLoc);
    HYPRE_IJVectorSetValues(b, n_,
        reinterpret_cast<const HYPRE_BigInt*>(rows_.data()), r.data());
    HYPRE_IJVectorAssemble(b);

    Kokkos::deep_copy(z, 0.0);  // zero initial guess
    HYPRE_IJVectorInitialize_v2(x, kMemLoc);
    HYPRE_IJVectorSetValues(x, n_,
        reinterpret_cast<const HYPRE_BigInt*>(rows_.data()), z.data());
    HYPRE_IJVectorAssemble(x);

    HYPRE_ParCSRMatrix parA;
    HYPRE_IJMatrixGetObject(static_cast<HYPRE_IJMatrix>(A_), reinterpret_cast<void**>(&parA));
    HYPRE_ParVector parb, parx;
    HYPRE_IJVectorGetObject(b, reinterpret_cast<void**>(&parb));
    HYPRE_IJVectorGetObject(x, reinterpret_cast<void**>(&parx));
    HYPRE_BoomerAMGSolve(static_cast<HYPRE_Solver>(amg_), parA, parb, parx);

    HYPRE_IJVectorGetValues(x, n_,
        reinterpret_cast<const HYPRE_BigInt*>(rows_.data()), z.data());
}

void KokkosAmgPreconditioner::finalize() {
    if (amg_) { HYPRE_BoomerAMGDestroy(static_cast<HYPRE_Solver>(amg_)); amg_ = nullptr; }
    if (A_)   { HYPRE_IJMatrixDestroy(static_cast<HYPRE_IJMatrix>(A_));  A_ = nullptr; }
    if (b_)   { HYPRE_IJVectorDestroy(static_cast<HYPRE_IJVector>(b_));  b_ = nullptr; }
    if (x_)   { HYPRE_IJVectorDestroy(static_cast<HYPRE_IJVector>(x_));  x_ = nullptr; }
    n_ = 0;
    hierarchy_built_ = false;
}

KokkosAmgPreconditioner::~KokkosAmgPreconditioner() { finalize(); }

} // namespace openswmm::twoD::gpu
