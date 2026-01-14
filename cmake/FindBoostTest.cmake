# FindBoostTest.cmake - Modern CMake module to fetch Boost.Test headers
#
# This module fetches Boost.Test and its dependencies as header-only libraries
# and creates an INTERFACE target: boost_test_headers
#
# Usage:
#   find_package(BoostTest REQUIRED)
#   target_link_libraries(my_target PRIVATE boost_test_headers)

# Prevent duplicate inclusion
if(TARGET boost_test_headers)
    set(BoostTest_FOUND TRUE)
    return()
endif()

message(STATUS "Fetching Boost.Test headers")

include(FetchContent)

# Boost version to fetch
set(BOOST_VERSION "1.88.0" CACHE STRING "Boost version to fetch")

# Boost.Test dependencies (header-only libraries needed)
set(BOOST_TEST_DEPS
    algorithm
    assert
    bind
    config
    core
    detail
    exception
    function
    io
    iterator
    move
    mp11
    mpl
    numeric_conversion
    preprocessor
    range
    smart_ptr
    static_assert
    test
    throw_exception
    type_traits
    utility
)

# Declare all Boost dependencies
foreach(lib IN LISTS BOOST_TEST_DEPS)
    message(VERBOSE "Declaring boost::${lib}")
    
    FetchContent_Declare(
        boost_${lib}
        URL "https://github.com/boostorg/${lib}/archive/refs/tags/boost-${BOOST_VERSION}.tar.gz"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SYSTEM
    )
endforeach()

# Make all dependencies available
FetchContent_MakeAvailable(${BOOST_TEST_DEPS})

# Create INTERFACE library with all include paths
add_library(boost_test_headers INTERFACE)
add_library(Boost::test_headers ALIAS boost_test_headers)

# Add include directories from all fetched Boost libraries
foreach(lib IN LISTS BOOST_TEST_DEPS)
    # FetchContent_MakeAvailable creates lowercase targets
    FetchContent_GetProperties(boost_${lib} SOURCE_DIR boost_${lib}_SOURCE_DIR)
    
    if(boost_${lib}_SOURCE_DIR)
        target_include_directories(boost_test_headers
            INTERFACE
                $<BUILD_INTERFACE:${boost_${lib}_SOURCE_DIR}/include>
        )
    else()
        message(WARNING "Failed to get source directory for boost::${lib}")
    endif()
endforeach()

# Mark library as system headers to suppress warnings
target_compile_options(boost_test_headers
    INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-isystem>
)

# Set standard find_package variables
set(BoostTest_FOUND TRUE)
set(BoostTest_VERSION ${BOOST_VERSION})

message(STATUS "Boost.Test headers (v${BOOST_VERSION}) configured successfully")

# Provide package components for future extensibility
set(BoostTest_COMPONENTS headers)