# Headless: builds the menu model with GLib/GObject only, no display needed.
add_executable(wordsworth-menu-bar-tests
    test-menu-bar.c
)

target_link_libraries(wordsworth-menu-bar-tests PRIVATE
    gtk4_lib
)

add_test(NAME wordsworth-menu-bar-tests COMMAND wordsworth-menu-bar-tests)
