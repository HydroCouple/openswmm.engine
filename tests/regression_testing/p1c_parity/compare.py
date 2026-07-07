#!/usr/bin/env python3
"""P1-C parity: serial CvodeSurfaceSolver vs openswmm_gpu_omp (Kokkos OpenMP).

Reports per-dataset max |abs| and max |rel| difference and checks agreement
against the CVODE solver tolerance (atol=1e-6, rtol=1e-4 from SolverOptions2D).
Not bit-identical is expected (GMRES parallel reductions reassociate FP); the
claim under test is tolerance-level agreement.
"""
import sys
import h5py
import numpy as np

ATOL, RTOL = 1.0e-6, 1.0e-4  # SolverOptions2D defaults

a = h5py.File("serial.h5", "r")
b = h5py.File("gpu.h5", "r")

# Physical result fields (skip static geometry/topology which are bit-identical).
fields = [
    "Mesh2_face_depth", "Mesh2_face_head", "Mesh2_face_vx", "Mesh2_face_vy",
    "Mesh2_node_head", "Mesh2_face_max_depth", "Mesh2_face_max_velocity",
    "Mesh2_face_max_continuity_err", "Mesh2_edge_flux",
]

print(f"{'dataset':32s} {'max|abs|':>12s} {'max|rel|':>12s} {'within tol?':>12s}")
print("-" * 72)
all_ok = True
for f in fields:
    if f not in a or f not in b:
        print(f"{f:32s} {'MISSING':>12s}")
        continue
    x = np.asarray(a[f], dtype=np.float64).ravel()
    y = np.asarray(b[f], dtype=np.float64).ravel()
    if x.shape != y.shape:
        print(f"{f:32s} shape mismatch {x.shape} vs {y.shape}")
        all_ok = False
        continue
    adiff = np.abs(x - y)
    tol = ATOL + RTOL * np.abs(x)
    rdiff = adiff / np.maximum(np.abs(x), 1e-300)
    ok = bool(np.all(adiff <= tol))
    all_ok = all_ok and ok
    print(f"{f:32s} {adiff.max():12.3e} {rdiff.max():12.3e} "
          f"{('OK' if ok else 'EXCEEDS'):>12s}")

print("-" * 72)
print("PARITY WITHIN SOLVER TOLERANCE" if all_ok
      else "SOME FIELDS EXCEED atol+rtol*|ref|")
sys.exit(0 if all_ok else 1)
