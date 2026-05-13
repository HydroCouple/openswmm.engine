include_guard(GLOBAL)

# Apple-specific: try Homebrew locations
if(APPLE AND NOT OpenMP_FOUND)
    message(STATUS "Searching for Homebrew OpenMP...")
    
    # Common Homebrew paths
    set(HOMEBREW_PATHS
        /opt/homebrew/opt/libomp  # Apple Silicon
        /usr/local/opt/libomp     # Intel
    )
    
    foreach(HOMEBREW_PATH ${HOMEBREW_PATHS})
        if(EXISTS "${HOMEBREW_PATH}")
            message(STATUS "Found Homebrew libomp at ${HOMEBREW_PATH}")
            
            # Create imported targets
            if(NOT TARGET OpenMP::OpenMP_C)
                add_library(OpenMP::OpenMP_C INTERFACE IMPORTED)
                target_compile_options(OpenMP::OpenMP_C INTERFACE -Xpreprocessor -fopenmp)
                target_include_directories(OpenMP::OpenMP_C INTERFACE "${HOMEBREW_PATH}/include")
                target_link_libraries(OpenMP::OpenMP_C INTERFACE "${HOMEBREW_PATH}/lib/libomp.dylib")
            endif()
            
            if(NOT TARGET OpenMP::OpenMP_CXX)
                add_library(OpenMP::OpenMP_CXX INTERFACE IMPORTED)
                target_compile_options(OpenMP::OpenMP_CXX INTERFACE -Xpreprocessor -fopenmp)
                target_include_directories(OpenMP::OpenMP_CXX INTERFACE "${HOMEBREW_PATH}/include")
                target_link_libraries(OpenMP::OpenMP_CXX INTERFACE "${HOMEBREW_PATH}/lib/libomp.dylib")
            endif()
            
            set(OpenMP_FOUND TRUE PARENT_SCOPE)
            set(OpenMP_C_FOUND TRUE PARENT_SCOPE)
            set(OpenMP_CXX_FOUND TRUE PARENT_SCOPE)
            
            return()
        endif()
    endforeach()
    
    message(WARNING "OpenMP not found. Install via: brew install libomp")
endif()