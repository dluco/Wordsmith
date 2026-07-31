add_executable(wordsworth-driver
    wordsworth-driver.c
)

target_link_libraries(wordsworth-driver PRIVATE
    wordsworth-ui
)

set_target_properties(wordsworth-driver PROPERTIES
    OUTPUT_NAME wordsworth
)
