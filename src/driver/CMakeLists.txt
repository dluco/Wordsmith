add_executable(wordsmith-driver
    wordsmith-driver.c
)

target_link_libraries(wordsmith-driver PRIVATE
    wordsmith-ui
)

set_target_properties(wordsmith-driver PROPERTIES
    OUTPUT_NAME wordsmith
)
