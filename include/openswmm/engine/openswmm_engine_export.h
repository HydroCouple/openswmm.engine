
#ifndef SWMM_ENGINE_API_H
#define SWMM_ENGINE_API_H

#ifdef OPENSWMM_ENGINE_STATIC
#  define SWMM_ENGINE_API
#  define OPENSWMM_ENGINE_NO_EXPORT
#else
#  ifndef SWMM_ENGINE_API
#    ifdef openswmm_engine_EXPORTS
        /* We are building this library */
#      define SWMM_ENGINE_API __declspec(dllexport)
#    else
        /* We are using this library */
#      define SWMM_ENGINE_API __declspec(dllimport)
#    endif
#  endif

#  ifndef OPENSWMM_ENGINE_NO_EXPORT
#    define OPENSWMM_ENGINE_NO_EXPORT 
#  endif
#endif

#ifndef OPENSWMM_ENGINE_DEPRECATED
#  define OPENSWMM_ENGINE_DEPRECATED __declspec(deprecated)
#endif

#ifndef OPENSWMM_ENGINE_DEPRECATED_EXPORT
#  define OPENSWMM_ENGINE_DEPRECATED_EXPORT SWMM_ENGINE_API OPENSWMM_ENGINE_DEPRECATED
#endif

#ifndef OPENSWMM_ENGINE_DEPRECATED_NO_EXPORT
#  define OPENSWMM_ENGINE_DEPRECATED_NO_EXPORT OPENSWMM_ENGINE_NO_EXPORT OPENSWMM_ENGINE_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef OPENSWMM_ENGINE_NO_DEPRECATED
#    define OPENSWMM_ENGINE_NO_DEPRECATED
#  endif
#endif

#endif /* SWMM_ENGINE_API_H */
