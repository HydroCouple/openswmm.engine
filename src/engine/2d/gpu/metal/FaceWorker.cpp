// SPDX-License-Identifier: Apache-2.0
#include <Kokkos_Core.hpp>
#include <sycl/sycl.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#if !defined(KOKKOS_IMPL_SYCL_ADAPTIVECPP_METAL) || !defined(SYCL_EXT_ACPP_BACKEND_METAL)
#error The face worker requires Kokkos SYCL with AdaptiveCpp Metal
#endif
#include "FaceProtocol.hpp"
#define OPENSWMM_KERNEL_FN KOKKOS_INLINE_FUNCTION
#include "../../solver/SweKernels.hpp"
#include "../../../hydraulics/fv/FvKernels.hpp"

namespace openswmm::twoD::metal {
KOKKOS_INLINE_FUNCTION FaceOutput flux(FaceInput f, StepParams p) {
    if (f.h <= p.dry)
        return {0.f, 0.f};
    const float g = static_cast<float>(constants::GRAVITY_SI);
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
struct LocalInertialFlux {
    KOKKOS_INLINE_FUNCTION FaceOutput operator()(FaceInput f, StepParams p) const {
        return flux(f, p);
    }
};
struct SurfaceFlux {
    KOKKOS_INLINE_FUNCTION SurfaceOutput operator()(SurfaceInput f, StepParams p) const {
        swe::FaceFluxT<float> value;
        SurfaceOutput out{};
        swe::faceFlux(f.eta_left, f.depth_left, f.qx_left, f.qy_left, f.eta_right, f.depth_right,
                      f.qx_right, f.qy_right, f.nx, f.ny, p.dry, value, out.corr_lx, out.corr_ly,
                      out.corr_rx, out.corr_ry);
        out.mass = value.mass;
        out.mx = value.mx;
        out.my = value.my;
        return out;
    }
};
struct NetworkFlux {
    KOKKOS_INLINE_FUNCTION static openswmm::fv::kernels::FaceStateT<float> unpack(NetworkSide f) {
        return {f.area,     f.discharge,         f.velocity,
                f.celerity, f.pressure_integral, static_cast<uint8_t>(f.pressure_state)};
    }
    KOKKOS_INLINE_FUNCTION NetworkOutput operator()(NetworkInput f, StepParams) const {
        const auto value = openswmm::fv::kernels::riemannFlux(unpack(f.left), unpack(f.right));
        return {value.mass, value.mom};
    }
};
bool finiteFlux(FaceOutput f) {
    return std::isfinite(f.q) && std::isfinite(f.transfer);
}
bool finiteFlux(NetworkOutput f) {
    return std::isfinite(f.mass) && std::isfinite(f.momentum);
}
bool finiteFlux(SurfaceOutput f) {
    return std::isfinite(f.mass) && std::isfinite(f.mx) && std::isfinite(f.my) &&
           std::isfinite(f.corr_lx) && std::isfinite(f.corr_ly) && std::isfinite(f.corr_rx) &&
           std::isfinite(f.corr_ry);
}
using Space = Kokkos::SYCL;
template <class Input, class Output, class Evaluate>
void serve(const Space& space, FaceOperation op, Evaluate evaluate) {
    Kokkos::View<Input*, Space::memory_space> faces;
    Kokkos::View<Output*, Space::memory_space> out;
    Kokkos::View<Input*, Kokkos::HostSpace> input;
    Kokkos::View<Output*, Kokkos::HostSpace> output;
    std::uint64_t sweeps = 0, evaluations = 0;

    FaceRequest req;
    while (read_bytes(STDIN_FILENO, &req, sizeof(req), true)) {
        if (req.count == 0 || req.count > INT32_MAX)
            throw std::runtime_error("invalid face count");
        // Grow only: with LTS tiers each request is one tier's faces, so the
        // count changes on nearly every request.
        if (faces.extent(0) < req.count) {
            faces = decltype(faces)(Kokkos::view_alloc(Kokkos::WithoutInitializing, "faces"),
                                    req.count);
            out = decltype(out)(Kokkos::view_alloc(Kokkos::WithoutInitializing, "flux"), req.count);
            input = decltype(input)(Kokkos::view_alloc(Kokkos::WithoutInitializing, "inputs"),
                                    req.count);
            output = decltype(output)(Kokkos::view_alloc(Kokkos::WithoutInitializing, "outputs"),
                                      req.count);
        }
        const std::pair<std::size_t, std::size_t> used(0, req.count);
        read_bytes(STDIN_FILENO, input.data(), req.count * sizeof(Input));
        Kokkos::deep_copy(space, Kokkos::subview(faces, used), Kokkos::subview(input, used));
        const auto params = req.params;
        Kokkos::parallel_for(
            operation_name(op), Kokkos::RangePolicy<Space>(space, 0, req.count),
            KOKKOS_LAMBDA(int i) { out(i) = evaluate(faces(i), params); });
        Kokkos::deep_copy(space, Kokkos::subview(output, used), Kokkos::subview(out, used));
        space.fence();
        space.sycl_queue().wait_and_throw();
        for (std::uint32_t i = 0; i < req.count; ++i)
            if (!finiteFlux(output(i)))
                throw std::runtime_error("non-finite hydraulic face flux");
        write_bytes(STDIN_FILENO, output.data(), req.count * sizeof(Output));
        ++sweeps;
        evaluations += req.count;
    }
    std::fprintf(stderr, "face_worker_complete sweeps=%llu evaluations=%llu domain=%s\n",
                 static_cast<unsigned long long>(sweeps),
                 static_cast<unsigned long long>(evaluations), operation_name(op));
}
}
int main(int argc, char** argv) {
    using namespace openswmm::twoD::metal;
    try {
        FaceOperation op = FaceOperation::local_inertial;
        if (argc == 2) {
            if (std::strcmp(argv[1], "surface-fv") == 0)
                op = FaceOperation::surface_fv;
            else if (std::strcmp(argv[1], "network-fv") == 0)
                op = FaceOperation::network_fv;
            else if (std::strcmp(argv[1], "local-inertial") != 0)
                return 2;
        } else if (argc != 1)
            return 2;
        const auto devices = Kokkos::Impl::get_sycl_devices();
        const auto it = std::find_if(devices.begin(), devices.end(), [](const auto& d) {
#ifdef OPENSWMM_FACE_CPU_CONTROL
            return d.is_cpu();
#else
            return d.is_gpu() && d.get_backend() == sycl::backend::metal;
#endif
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
#ifdef OPENSWMM_FACE_CPU_CONTROL
        constexpr auto backend = "sycl-cpu-control";
#else
        constexpr auto backend = "metal";
#endif
        std::fprintf(stderr, "face_operator=%s queue_device=%s domain=%s\n", backend,
                     it->get_info<sycl::info::device::name>().c_str(), operation_name(op));

        const auto name = it->get_info<sycl::info::device::name>();
        WorkerHello hello{op, {}};
        std::snprintf(hello.device, sizeof(hello.device), "%s", name.c_str());
        write_bytes(STDIN_FILENO, &hello, sizeof(hello));
        switch (op) {
        case FaceOperation::local_inertial:
            serve<FaceInput, FaceOutput>(space, op, LocalInertialFlux{});
            break;
        case FaceOperation::surface_fv:
            serve<SurfaceInput, SurfaceOutput>(space, op, SurfaceFlux{});
            break;
        case FaceOperation::network_fv:
            serve<NetworkInput, NetworkOutput>(space, op, NetworkFlux{});
            break;
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "face_worker_error=%s\n", e.what());
        return 1;
    }
}
