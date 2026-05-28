
####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was OpenSWMMEngineConfig.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################

include(CMakeFindDependencyMacro)

# Threads is required on all platforms: MSVC resolves it via the C runtime;
# GCC/Clang via -lpthread.  Declaring it here ensures the exported Targets
# file never references an undefined CMake target in downstream consumers.
find_dependency(Threads)

# Include the targets file
include("${CMAKE_CURRENT_LIST_DIR}/OpenSWMMEngineTargets.cmake")

# Provide portable variables for consumers.
# On Windows a shared-library build produces both an import library (.lib →
# CMAKE_INSTALL_LIBDIR) and a DLL (.dll → CMAKE_INSTALL_BINDIR).  Downstream
# projects needing the DLL at runtime should use OPENSWMMEngine_BIN_DIR;
# link-time consumers use OPENSWMMEngine_LIB_DIR.
set_and_check(OPENSWMMEngine_INCLUDE_DIR "${PACKAGE_PREFIX_DIR}/include")
set_and_check(OPENSWMMEngine_LIB_DIR     "${PACKAGE_PREFIX_DIR}/lib")
# BIN_DIR is optional: not created when only libraries are installed (e.g.
# Python wheel builds that don't include the openswmm CLI executable).
set(OPENSWMMEngine_BIN_DIR "")

# Set convenience variables
set(OPENSWMMEngine_INCLUDE_DIRS "${OPENSWMMEngine_INCLUDE_DIR}")
set(OPENSWMMEngine_LIBRARY_DIRS "${OPENSWMMEngine_LIB_DIR}")
set(OPENSWMMEngine_VERSION "6.0.0")

# Check required components
check_required_components(OPENSWMMEngine)

