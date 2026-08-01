function(wordsmith_add_core_test name source)
    add_executable(${name} ${source})

    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )

    target_link_libraries(${name} PRIVATE wordsmith-core)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)

    add_test(NAME ${name} COMMAND ${name})
endfunction()

wordsmith_add_core_test(wordsmith-version-tests test-version.cpp)
wordsmith_add_core_test(wordsmith-markup-tests test-markup.cpp)
wordsmith_add_core_test(wordsmith-project-tests test-project.cpp)
wordsmith_add_core_test(wordsmith-safe-write-tests test-safe-write.cpp)
wordsmith_add_core_test(wordsmith-yaml-tests test-yaml.cpp)
wordsmith_add_core_test(wordsmith-frontmatter-tests test-frontmatter.cpp)
