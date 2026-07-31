cmake_minimum_required(VERSION 3.31)

project(Wordsworth
    VERSION 0.0.0
    LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Disable sub-project tests/docs by default. Wordsworth vendors these libs.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)

option(WORDSWORTH_BUILD_TESTS "Build Wordsworth's own test executables" ON)
if (WORDSWORTH_BUILD_TESTS)
    enable_testing()
endif ()

add_subdirectory(external/argo)

add_subdirectory(src)

if (WORDSWORTH_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
