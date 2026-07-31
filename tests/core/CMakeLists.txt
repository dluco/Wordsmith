function(wordsworth_add_core_test name source)
    add_executable(${name} ${source})

    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )

    target_link_libraries(${name} PRIVATE wordsworth-core)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)

    add_test(NAME ${name} COMMAND ${name})
endfunction()

wordsworth_add_core_test(wordsworth-version-tests test-version.cpp)
wordsworth_add_core_test(wordsworth-markup-tests test-markup.cpp)
wordsworth_add_core_test(wordsworth-project-tests test-project.cpp)
