// SPDX-License-Identifier: Apache-2.0
#include <Kokkos_Core.hpp>
#include <sycl/sycl.hpp>
#include <algorithm>
#include <cstdio>

#if !defined(KOKKOS_IMPL_SYCL_ADAPTIVECPP_METAL) || !defined(SYCL_EXT_ACPP_BACKEND_METAL)
#error The face worker requires Kokkos SYCL with AdaptiveCpp Metal
#endif
#include "FaceProtocol.hpp"

namespace openswmm::twoD::metal {
KOKKOS_INLINE_FUNCTION FaceOutput flux(FaceInput f, StepParams p) {
    if (f.h <= p.dry)
        return {0.f, 0.f};
    const float g = 9.80665f;
    const float h73 = f.h * f.h * Kokkos::pow(f.h, 1.f / 3.f);
    float q = (f.qhat - p.dt * (g * f.h * f.slope)) / (1.f + g * p.dt * f.n2 * f.qmag / h73);
    const float cap = p.fr * f.h * Kokkos::sqrt(g * f.h);
    if (q > cap)
        q = cap;
    if (q < -cap)
        q = -cap;
    const float budget = q > 0.f ? f.leftBudget : f.rightBudget;
    const float take = (q > 0.f ? q : -q) * f.width * p.dt;
    if (take > budget && take > 0.f)
        q *= budget / take;
    return {q, q * f.width * p.dt};
}
}

int main() {
    using namespace openswmm::twoD::metal;
    try {
        const auto devices = Kokkos::Impl::get_sycl_devices();
        const auto it = std::find_if(devices.begin(), devices.end(), [](const auto& d) {
            return d.is_gpu() && d.get_backend() == sycl::backend::metal;
        });
        if (it == devices.end()) {
            std::fprintf(stderr, "UNAVAILABLE: no Metal GPU for OpenSWMM face flow\n");
            return no_metal_device;
        }
        Kokkos::ScopeGuard guard(
            Kokkos::InitializationSettings{}.set_device_id(static_cast<int>(it - devices.begin())));
        using Space = Kokkos::SYCL;
        const Space space;
        if (space.sycl_queue().get_device() != *it)
            throw std::runtime_error("unexpected face queue device");
        std::fprintf(stderr, "face_operator=metal queue_device=%s\n",
                     it->get_info<sycl::info::device::name>().c_str());

        Kokkos::View<FaceInput*, Space::memory_space> faces;
        Kokkos::View<FaceOutput*, Space::memory_space> out;
        Kokkos::View<FaceInput*, Kokkos::HostSpace> input;
        Kokkos::View<FaceOutput*, Kokkos::HostSpace> output;
        std::uint64_t sweeps = 0, evaluations = 0;
        write_bytes(STDIN_FILENO, &worker_ready, sizeof(worker_ready));
        FaceRequest req;
        while (read_bytes(STDIN_FILENO, &req, sizeof(req), true)) {
            if (req.count == 0 || req.count > INT32_MAX)
                throw std::runtime_error("invalid face count");
            if (faces.extent(0) != req.count) {
                faces = decltype(faces)(Kokkos::view_alloc(Kokkos::WithoutInitializing, "faces"),
                                        req.count);
                out = decltype(out)(Kokkos::view_alloc(Kokkos::WithoutInitializing, "flux"),
                                    req.count);
                input = decltype(input)(Kokkos::view_alloc(Kokkos::WithoutInitializing, "inputs"),
                                        req.count);
                output = decltype(output)(
                    Kokkos::view_alloc(Kokkos::WithoutInitializing, "outputs"), req.count);
            }
            read_bytes(STDIN_FILENO, input.data(), req.count * sizeof(FaceInput));
            Kokkos::deep_copy(space, faces, input);
            const auto params = req.params;
            Kokkos::parallel_for(
                "openswmm_live_flux", Kokkos::RangePolicy<Space>(space, 0, req.count),
                KOKKOS_LAMBDA(int i) { out(i) = flux(faces(i), params); });
            Kokkos::deep_copy(space, output, out);
            space.fence();
            space.sycl_queue().wait_and_throw();
            write_bytes(STDIN_FILENO, output.data(), req.count * sizeof(FaceOutput));
            ++sweeps;
            evaluations += req.count;
        }
        std::fprintf(stderr, "face_worker_complete sweeps=%llu evaluations=%llu\n",
                     static_cast<unsigned long long>(sweeps),
                     static_cast<unsigned long long>(evaluations));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "face_worker_error=%s\n", e.what());
        return 1;
    }
}
