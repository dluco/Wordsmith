add_executable(wordsworth-core-tests
    test-version.cpp
)

set_target_properties(wordsworth-core-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_link_libraries(wordsworth-core-tests PRIVATE
    wordsworth-core
)

target_include_directories(wordsworth-core-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME wordsworth-core-tests COMMAND wordsworth-core-tests)
