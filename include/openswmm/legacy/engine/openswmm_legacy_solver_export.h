
#ifndef EXPORT_OPENSWMMCORE_SOLVER_API_H
#define EXPORT_OPENSWMMCORE_SOLVER_API_H

#ifdef SHARED_EXPORTS_BUILT_AS_STATIC
#  define EXPORT_OPENSWMMCORE_SOLVER_API
#  define OPENSWMM_LEGACY_SOLVER_NO_EXPORT
#else
#  ifndef EXPORT_OPENSWMMCORE_SOLVER_API
#    ifdef openswmm_legacy_engine_EXPORTS
        /* We are building this library */
#      define EXPORT_OPENSWMMCORE_SOLVER_API __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define EXPORT_OPENSWMMCORE_SOLVER_API __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef OPENSWMM_LEGACY_SOLVER_NO_EXPORT
#    define OPENSWMM_LEGACY_SOLVER_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef OPENSWMM_LEGACY_SOLVER_DEPRECATED
#  define OPENSWMM_LEGACY_SOLVER_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef OPENSWMM_LEGACY_SOLVER_DEPRECATED_EXPORT
#  define OPENSWMM_LEGACY_SOLVER_DEPRECATED_EXPORT EXPORT_OPENSWMMCORE_SOLVER_API OPENSWMM_LEGACY_SOLVER_DEPRECATED
#endif

#ifndef OPENSWMM_LEGACY_SOLVER_DEPRECATED_NO_EXPORT
#  define OPENSWMM_LEGACY_SOLVER_DEPRECATED_NO_EXPORT OPENSWMM_LEGACY_SOLVER_NO_EXPORT OPENSWMM_LEGACY_SOLVER_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef OPENSWMM_LEGACY_SOLVER_NO_DEPRECATED
#    define OPENSWMM_LEGACY_SOLVER_NO_DEPRECATED
#  endif
#endif

#endif /* EXPORT_OPENSWMMCORE_SOLVER_API_H */
