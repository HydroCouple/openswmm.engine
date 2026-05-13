# ============================================================================
# PatchNumpyPxd.cmake
#
# Why this file exists
# --------------------
# NumPy 2.0 removed the `subarray` field from the public C struct
# `_PyArray_Descr`; access now goes through the inline accessor function
# `PyDataType_SUBARRAY(descr)`.  But NumPy still ships a Cython declaration
# file (`numpy/__init__.cython-30.pxd`) whose helper `PyDataType_SHAPE`
# accesses the field directly:
#
#     cdef inline tuple PyDataType_SHAPE(dtype d):
#         if PyDataType_HASSUBARRAY(d):
#             return <tuple>d.subarray.shape   ← compiles only on numpy 1.x
#         else:
#             return ()
#
# That's a `cdef inline` so Cython emits the function body into every
# translation unit that `cimport numpy`s — which is every single openswmm
# Cython extension.  Result: compile-time error
#     error: no member named 'subarray' in '_PyArray_Descr'
# whenever the module is built against a NumPy 2.x install.
#
# We work around this without touching the user's NumPy install: at CMake
# configure time, copy the numpy pxd files into a build-only directory and
# rewrite the broken line to use the accessor.  Cython's `--include-dir` is
# then prepended with that directory so its lookup order picks our patched
# copy ahead of NumPy's installed copy.
#
# When upstream numpy ships a fix, we can remove this file and the include
# in python/CMakeLists.txt — and the build is identical.
# ============================================================================

include_guard(GLOBAL)

# ----------------------------------------------------------------------------
# openswmm_patch_numpy_pxd()
#
# Inputs (read from caller's scope):
#   Python_EXECUTABLE — interpreter whose numpy install is patched
#
# Outputs (set in caller's scope):
#   OPENSWMM_NUMPY_PATCHED_INCLUDE_DIR — directory to prepend on Cython's
#                                        --include-dir.  Contents:
#                                          numpy/__init__.cython-30.pxd
#                                          numpy/__init__.pxd          (legacy)
#                                          numpy/__init__.cython-31.pxd (if present upstream)
# ----------------------------------------------------------------------------
function(openswmm_patch_numpy_pxd)
    if(NOT Python_EXECUTABLE)
        message(FATAL_ERROR "openswmm_patch_numpy_pxd: Python_EXECUTABLE must be set first")
    endif()

    # Resolve the numpy package directory in the active interpreter.
    execute_process(
        COMMAND "${Python_EXECUTABLE}" -c
            "import os, numpy; print(os.path.dirname(numpy.__file__))"
        OUTPUT_VARIABLE _numpy_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _rc
    )
    if(NOT _rc EQUAL 0 OR NOT EXISTS "${_numpy_dir}")
        message(FATAL_ERROR "Could not locate numpy install for ${Python_EXECUTABLE}")
    endif()

    # Resolve numpy version so we can short-circuit the patch on numpy 1.x
    # (the field exists there, no rewrite needed).
    execute_process(
        COMMAND "${Python_EXECUTABLE}" -c
            "import numpy; print(numpy.__version__)"
        OUTPUT_VARIABLE _numpy_version
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    set(_patched_dir "${CMAKE_BINARY_DIR}/numpy_patched")
    file(MAKE_DIRECTORY "${_patched_dir}/numpy")

    # The exact substring we're rewriting.  Stable across numpy 2.x point
    # releases as of 2025; if upstream changes the helper, the substitution
    # silently no-ops because the source string is gone.
    set(_old "<tuple>d.subarray.shape")
    set(_new "<tuple>PyDataType_SUBARRAY(d).shape")

    set(_patched_files "")
    foreach(_pxd "__init__.pxd" "__init__.cython-30.pxd" "__init__.cython-31.pxd")
        set(_src "${_numpy_dir}/${_pxd}")
        if(EXISTS "${_src}")
            set(_dst "${_patched_dir}/numpy/${_pxd}")
            file(READ "${_src}" _content)
            string(REPLACE "${_old}" "${_new}" _content "${_content}")
            file(WRITE "${_dst}" "${_content}")
            list(APPEND _patched_files "${_pxd}")
        endif()
    endforeach()

    if(_patched_files)
        message(STATUS "openswmm: patched numpy pxd files (${_patched_files}) "
                       "for numpy ${_numpy_version} → ${_patched_dir}")
    else()
        message(WARNING "openswmm: numpy ${_numpy_version} pxd files not found in "
                        "${_numpy_dir} — skipping patch (build may fail)")
    endif()

    set(OPENSWMM_NUMPY_PATCHED_INCLUDE_DIR "${_patched_dir}" PARENT_SCOPE)
endfunction()
