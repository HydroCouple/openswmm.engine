# openswmm-gpu-omp

OpenMP (Kokkos) acceleration companion for the [OpenSWMM](https://pypi.org/project/openswmm/)
2D surface solver.

The base `openswmm` wheel is CPU-only and fully portable (Kokkos-free). This
companion ships a single native plugin (`libopenswmm_gpu_omp`) that the OpenSWMM
engine discovers and loads at runtime to run the 2D surface solver on the
Kokkos-OpenMP backend.

## Install

```bash
pip install openswmm            # CPU-only base engine
pip install openswmm-gpu-omp    # add OpenMP acceleration
```

That is all. There is no public Python API: at import, the base `openswmm`
package detects this companion and adds it to the engine's plugin search path
(`OPENSWMM_GPU_PLUGIN_PATH`). With `OPENSWMM_2D_BACKEND` left at its `auto`
default, 2D surface runs then use OpenMP automatically. Setting
`OPENSWMM_2D_BACKEND=cpu` forces the serial solver; uninstalling this package
reverts to it permanently.

## Notes

- OpenMP only — no GPU driver required. Numerics match the serial CPU solver to
  ~1e-13 (not bit-identical); pin `OPENSWMM_2D_BACKEND=cpu` where bit-exact
  reproducibility is required.
- The plugin is version-locked to the base engine (`openswmm == <same version>`)
  and additionally ABI-checked at load time.

See `docs/2D_GPU_DEFAULTS.md` in the engine repository for the full
backend-selection matrix.
