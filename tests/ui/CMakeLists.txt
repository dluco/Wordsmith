# Headless: builds the menu model with GLib/GObject only, no display needed.
add_executable(wordsmith-menu-bar-tests
    test-menu-bar.c
)

target_link_libraries(wordsmith-menu-bar-tests PRIVATE
    gtk4_lib
)

add_test(NAME wordsmith-menu-bar-tests COMMAND wordsmith-menu-bar-tests)
