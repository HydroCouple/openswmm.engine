// Falsifier for the thread_local OdeWorkspace leak in src/engine/math/OdeSolver.cpp.
//
// ensureWorkspace() calloc's six buffers into a thread_local OdeWorkspace and
// frees them only when growing.  Before the fix the struct was trivially
// destructible, so no thread-exit destructor was registered: thread exit
// reclaimed the TLS block holding the *pointers* and stranded the buffers.
//
// The real call sites use n = 1 (runoff ponded depth, Runoff.cpp) and n = 2
// (groundwater, Groundwater.cpp), i.e. ~160 bytes per thread -- far too small
// to separate from allocator noise.  This probe drives a large n so each
// thread's workspace is 80*n bytes = 5 MiB, and runs threads sequentially
// (create/integrate/join) so a leak accumulates once per exited thread.
//
// LeakSanitizer cannot be used here: "detect_leaks is not supported on this
// platform" on Darwin arm64, which is why the existing ASan build never caught
// this.  So measure the allocator directly instead.

#include "OdeSolver.hpp"

#include <mach/mach.h>
#include <malloc/malloc.h>

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

constexpr int kN       = 65536;  // doubles per equation array
constexpr int kThreads = 16;     // sequential create/join cycles

// Bytes the workspace holds for a given n: five n-arrays plus the flat n*5 ak.
constexpr double kWorkspaceBytes = 80.0 * kN;

size_t heapBytesUsed() {
    return mstats().bytes_used;
}

size_t rssBytes() {
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return info.resident_size;
}

// One full integration on the calling thread, touching every workspace buffer.
void integrateOnce() {
    std::vector<double> y(kN, 1.0);
    const int rc = openswmm::ode::integrate(
        y.data(), kN, 0.0, 1.0, 1.0e-6, 1.0,
        [](double, const double*, double* dydx) {
            for (int i = 0; i < kN; ++i) dydx[i] = 0.0;
        });
    if (rc != openswmm::ode::ODE_OK) {
        std::fprintf(stderr, "integrate() returned %d, expected ODE_OK\n", rc);
        std::abort();
    }
}

double toMiB(long long bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

}  // namespace

int main() {
    // Warm-up thread amortises one-time runtime/TLS/allocator first-touch so the
    // baseline below is steady state rather than startup.
    {
        std::thread t(integrateOnce);
        t.join();
    }

    const size_t heap0 = heapBytesUsed();
    const size_t rss0  = rssBytes();

    for (int i = 0; i < kThreads; ++i) {
        std::thread t(integrateOnce);
        t.join();
    }

    const size_t heap1 = heapBytesUsed();
    const size_t rss1  = rssBytes();

    const double heapDeltaMiB = toMiB(static_cast<long long>(heap1) -
                                      static_cast<long long>(heap0));
    const double rssDeltaMiB  = toMiB(static_cast<long long>(rss1) -
                                      static_cast<long long>(rss0));
    const double expectedMiB  = toMiB(static_cast<long long>(
                                    kThreads * kWorkspaceBytes));

    std::printf("n per integrate      : %d\n", kN);
    std::printf("workspace per thread : %.2f MiB\n", toMiB(static_cast<long long>(kWorkspaceBytes)));
    std::printf("threads (seq)        : %d\n", kThreads);
    std::printf("expected if leaking  : %.2f MiB\n", expectedMiB);
    std::printf("heap delta (mstats)  : %.2f MiB\n", heapDeltaMiB);
    std::printf("rss delta            : %.2f MiB\n", rssDeltaMiB);

    // A leak lands near expectedMiB; a clean run lands near zero.  The gap is
    // ~80 MiB, so a quarter-of-expected threshold is nowhere near either edge.
    const bool leaking = heapDeltaMiB > expectedMiB * 0.25;
    std::printf("VERDICT              : %s\n", leaking ? "LEAKING" : "CLEAN");
    return leaking ? 1 : 0;
}
