# Headless: builds the menu model with GLib/GObject only, no display needed.
add_executable(wordsmith-menu-bar-tests
    test-menu-bar.c
)

target_link_libraries(wordsmith-menu-bar-tests PRIVATE
    gtk4_lib
)

add_test(NAME wordsmith-menu-bar-tests COMMAND wordsmith-menu-bar-tests)

# Also headless: text_scale_css() only builds a string, and GtkCssProvider
# parses one without a display.
add_executable(wordsmith-text-scale-tests
    test-text-scale.c
)

target_link_libraries(wordsmith-text-scale-tests PRIVATE
    wordsmith-ui
    gtk4_lib
)

target_include_directories(wordsmith-text-scale-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME wordsmith-text-scale-tests COMMAND wordsmith-text-scale-tests)

# Headless too: editor_composition_margin() is arithmetic over a width, which
# is why it is split out of the resize handler that applies it.
add_executable(wordsmith-composition-tests
    test-composition.c
)

target_link_libraries(wordsmith-composition-tests PRIVATE
    wordsmith-ui
    gtk4_lib
)

target_include_directories(wordsmith-composition-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME wordsmith-composition-tests COMMAND wordsmith-composition-tests)

# Headless as well: editor_style_flags() reads a GtkTextBuffer and its tags,
# both plain objects, which is why the format bar's rule lives outside the
# widget that draws it.
add_executable(wordsmith-format-bar-tests
    test-format-bar.c
)

target_link_libraries(wordsmith-format-bar-tests PRIVATE
    wordsmith-ui
    gtk4_lib
)

target_include_directories(wordsmith-format-bar-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME wordsmith-format-bar-tests COMMAND wordsmith-format-bar-tests)
