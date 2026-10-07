from pathlib import Path
import shutil
p=Path(__file__).resolve().parent;shutil.copytree(p/'src_direct',p/'src_inlined',dirs_exist_ok=True);f=p/'src_inlined/src/engine/2d/solver/ExplicitInertialSolver.cpp';s=f.read_text();s=s.replace('#include "SweKernels.hpp"','''// Inline the small SWE kernel chain in this CPU translation unit. Keep the
// shared header's annotation unchanged for other backends and callers.
#pragma push_macro("OPENSWMM_KERNEL_FN")
#undef OPENSWMM_KERNEL_FN
#if defined(_MSC_VER)
#define OPENSWMM_KERNEL_FN __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define OPENSWMM_KERNEL_FN inline __attribute__((always_inline))
#else
#define OPENSWMM_KERNEL_FN inline
#endif
#include "SweKernels.hpp"
#pragma pop_macro("OPENSWMM_KERNEL_FN")''');f.write_text(s)
