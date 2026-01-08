#
# FindOpenMP.cmake - Modern OpenMP detection with FetchContent fallback
#
# This module provides modern OpenMP detection with Apple Silicon support
# and falls back to building LLVM OpenMP if not found system-wide.
#

include_guard(GLOBAL)

# Try standard OpenMP detection first
find_package(OpenMP QUIET)
if(OpenMP_FOUND)
    return()
endif()

# Apple-specific OpenMP handling with FetchContent fallback
if(APPLE AND NOT OpenMP_FOUND)
    message(STATUS "OpenMP not found system-wide, using FetchContent to build LLVM OpenMP...")
    
    include(FetchContent)
    
    # Fetch LLVM OpenMP
    FetchContent_Declare(
        llvm_openmp
        URL https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/openmp-18.1.8.src.tar.xz
        URL_HASH SHA256=2280df189fd1bb3878ce85316a5a70d6a1e0c5e1b13e00c0e1c9064d4e0012fc
    )
    
    # OpenMP build options
    set(OPENMP_STANDALONE_BUILD TRUE)
    set(LIBOMP_INSTALL_ALIASES OFF)
    set(OPENMP_ENABLE_LIBOMPTARGET OFF)  # Disable GPU support for faster builds
    
    FetchContent_MakeAvailable(llvm_openmp)
    
    # Create modern imported targets if they don't exist
    if(NOT TARGET OpenMP::OpenMP_C)
        add_library(OpenMP::OpenMP_C INTERFACE IMPORTED)
        target_compile_options(OpenMP::OpenMP_C INTERFACE
            $<$<COMPILE_LANGUAGE:C>:-Xpreprocessor -fopenmp>
        )
        target_include_directories(OpenMP::OpenMP_C INTERFACE
            $<BUILD_INTERFACE:${llvm_openmp_SOURCE_DIR}/runtime/src>
        )
        target_link_libraries(OpenMP::OpenMP_C INTERFACE omp)
    endif()
    
    if(NOT TARGET OpenMP::OpenMP_CXX)
        add_library(OpenMP::OpenMP_CXX INTERFACE IMPORTED)
        target_compile_options(OpenMP::OpenMP_CXX INTERFACE
            $<$<COMPILE_LANGUAGE:CXX>:-Xpreprocessor -fopenmp>
        )
        target_include_directories(OpenMP::OpenMP_CXX INTERFACE
            $<BUILD_INTERFACE:${llvm_openmp_SOURCE_DIR}/runtime/src>
        )
        target_link_libraries(OpenMP::OpenMP_CXX INTERFACE omp)
    endif()
    
    # Set standard OpenMP variables for compatibility
    set(OpenMP_FOUND TRUE)
    set(OpenMP_C_FOUND TRUE)
    set(OpenMP_CXX_FOUND TRUE)
    set(OpenMP_VERSION "18.1.8")
    
    # Export for parent scope
    set(OpenMP_FOUND ${OpenMP_FOUND} PARENT_SCOPE)
    set(OpenMP_C_FOUND ${OpenMP_C_FOUND} PARENT_SCOPE)
    set(OpenMP_CXX_FOUND ${OpenMP_CXX_FOUND} PARENT_SCOPE)
    set(OpenMP_VERSION ${OpenMP_VERSION} PARENT_SCOPE)
    
    message(STATUS "Built LLVM OpenMP ${OpenMP_VERSION} from source")
endif()

# Final check
if(NOT OpenMP_FOUND)
    message(FATAL_ERROR "OpenMP not found and could not be built from source")
endif()