include_guard(GLOBAL)

# Apple-specific: try Homebrew locations.
#
# Gate on the imported TARGET, not the cached OpenMP_FOUND bool. CMake caches
# OpenMP_FOUND (we set it CACHE INTERNAL below) but does NOT persist imported
# targets across configure runs — they are recreated each run. So on any
# reconfigure of an existing build tree, OpenMP_FOUND is already TRUE while
# OpenMP::OpenMP_CXX does not yet exist; gating on `NOT OpenMP_FOUND` then
# skips this body and the target is never recreated, so every downstream
# `target_link_libraries(... OpenMP::OpenMP_CXX)` (and Kokkos' exported
# KokkosTargets link interface) fails with "target was not found". Gating on
# `NOT TARGET` recreates it whenever it is missing; the inner per-target
# `NOT TARGET` guards keep this idempotent if a target already exists.
if(APPLE AND NOT TARGET OpenMP::OpenMP_CXX)
    message(STATUS "Searching for Homebrew OpenMP...")
    
    # Common Homebrew paths
    set(HOMEBREW_PATHS
        /opt/homebrew/opt/libomp  # Apple Silicon
        /usr/local/opt/libomp     # Intel
    )
    
    foreach(HOMEBREW_PATH ${HOMEBREW_PATHS})
        if(EXISTS "${HOMEBREW_PATH}")
            message(STATUS "Found Homebrew libomp at ${HOMEBREW_PATH}")
            
            # Create imported targets. GLOBAL so they outlive the directory
            # scope of whichever CMakeLists.txt includes this finder first.
            # include_guard(GLOBAL) above means the finder body runs exactly
            # once (the legacy engine includes it before src/engine); without
            # GLOBAL the targets would be visible only in that first directory
            # subtree, so the src/engine consume site — and the transitive
            # SUNDIALS -> Kokkos find_dependency(OpenMP REQUIRED) it triggers —
            # would not see OpenMP::OpenMP_CXX and would fail to configure.
            if(NOT TARGET OpenMP::OpenMP_C)
                add_library(OpenMP::OpenMP_C INTERFACE IMPORTED GLOBAL)
                target_compile_options(OpenMP::OpenMP_C INTERFACE -Xpreprocessor -fopenmp)
                target_include_directories(OpenMP::OpenMP_C INTERFACE "${HOMEBREW_PATH}/include")
                target_link_libraries(OpenMP::OpenMP_C INTERFACE "${HOMEBREW_PATH}/lib/libomp.dylib")
            endif()

            if(NOT TARGET OpenMP::OpenMP_CXX)
                add_library(OpenMP::OpenMP_CXX INTERFACE IMPORTED GLOBAL)
                target_compile_options(OpenMP::OpenMP_CXX INTERFACE -Xpreprocessor -fopenmp)
                target_include_directories(OpenMP::OpenMP_CXX INTERFACE "${HOMEBREW_PATH}/include")
                target_link_libraries(OpenMP::OpenMP_CXX INTERFACE "${HOMEBREW_PATH}/lib/libomp.dylib")
            endif()

            # Cache (INTERNAL) so the found-state is visible in every scope, not
            # just the caller's. This both satisfies the if(OpenMP_FOUND) gate in
            # the including CMakeLists and lets a later find_package(OpenMP)
            # (e.g. KokkosConfig's find_dependency) report OpenMP as found after
            # include_guard short-circuits the re-run of this module.
            set(OpenMP_FOUND TRUE CACHE INTERNAL "OpenMP located via Homebrew libomp")
            set(OpenMP_C_FOUND TRUE CACHE INTERNAL "OpenMP C located via Homebrew libomp")
            set(OpenMP_CXX_FOUND TRUE CACHE INTERNAL "OpenMP CXX located via Homebrew libomp")

            return()
        endif()
    endforeach()
    
    message(WARNING "OpenMP not found. Install via: brew install libomp")
endif()