# Apple silicon Metal face solver

The `Darwin-metal` preset builds the host surface plugin with the engine's usual compiler and vcpkg Kokkos. A separate AdaptiveCpp build links the Metal face worker to the HydroCouple Kokkos fork. The plugin locates the worker beside its own dylib in both build and install layouts.

Use a Metal-enabled `acpp`, Ninja, and the standard arm64 engine vcpkg dependencies. From the engine root:

```sh
cmake --preset Darwin-metal -DOPENSWMM_ACPP_EXECUTABLE="$(command -v acpp)"
cmake --build --preset Darwin-metal --target openswmm openswmm_gpu_omp
OPENSWMM_2D_METAL_FACES=1 \
  build/darwin-metal/src/cli/openswmm model.inp model.rpt model.out
```

If the worker needs a different SDK from the host compiler, pass `-DOPENSWMM_METAL_SDK=/path/to/MacOSX.sdk`. On the development M1 Max, LLVM 19 used the macOS 26.5 SDK and a Metal-enabled AdaptiveCpp installation. Copying the worker alone is insufficient to deploy to another Mac. Its matching AdaptiveCpp runtime and Metal backend must also be available.

The worker resolves Kokkos from `OPENSWMM_KOKKOS_SOURCE_DIR`, a sibling `../kokkos` checkout, or a Git fetch at `cmake/KokkosMetalRevision.txt`, in that order. Local checkouts must include the fork's Apple Metal support. `OPENSWMM_KOKKOS_GIT_REPOSITORY` and `OPENSWMM_KOKKOS_GIT_TAG` override the fetch. On a fresh arm64 macOS configure, the host plugin enables the worker automatically when CMake finds an AdaptiveCpp installation with its Metal backend.

Select `FV_BACKEND METAL` in `[OPTIONS]` for first-order explicit network FV interior conduit fluxes. Select `BACKEND METAL` in `[2D_OPTIONS]` for first-order water-only surface fluxes with local-inertial or full shallow-water momentum and advection disabled, at any `LTS_TIERS`. Each tier's faces go to the worker as one request. Both selections can be active in a coupled model. On a Metal-enabled build, setting `OPENSWMM_2D_METAL_FACES=1` requires this path even on small meshes. Unsupported models and unavailable devices produce an error. Geometry, cell updates, and storage remain on the host. A run with a visible device prints `face_operator=metal queue_device=...` and completion counts. Cross-workgroup reductions and scans in the fork are still under qualification. The worker uses elementwise operations and copies.

The macOS workflow builds the preset, runs a small hydraulic case, and checks its diagnostic with the GPU hidden. Dispatch it with `hardware=true` to run the same case on Metal and verify the queue and completed face evaluations. That job uses a self-hosted runner labelled `macOS`, `ARM64`, and `metal`, with Metal-enabled `acpp` on PATH. Set `ACPP_APP_SDK` on the runner if its compiler needs a particular SDK.
