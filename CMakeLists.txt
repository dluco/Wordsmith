cmake_minimum_required(VERSION 3.31)

project(Wordsmith
    VERSION 0.0.0
    LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Disable sub-project tests/docs by default. Wordsmith vendors these libs.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)

option(WORDSMITH_BUILD_TESTS "Build Wordsmith's own test executables" ON)
if (WORDSMITH_BUILD_TESTS)
    enable_testing()
endif ()

add_subdirectory(external/argo)

add_subdirectory(src)

if (WORDSMITH_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
