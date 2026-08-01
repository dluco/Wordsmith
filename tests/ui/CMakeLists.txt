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
