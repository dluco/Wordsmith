find_package(PkgConfig QUIET REQUIRED)

# 4.12 for gtk_css_provider_load_from_string(), which text-scale.c uses to
# rebuild its rule; the load_from_data() it replaced is deprecated from there on.
pkg_check_modules(GTK4 QUIET REQUIRED gtk4>=4.12)

add_library(gtk4_lib INTERFACE IMPORTED GLOBAL)

target_include_directories(gtk4_lib INTERFACE
    ${GTK4_INCLUDE_DIRS}
)

target_compile_options(gtk4_lib INTERFACE
    ${GTK4_CFLAGS_OTHER}
)

target_link_libraries(gtk4_lib INTERFACE
    ${GTK4_LIBRARIES}
)

if (GTK4_LDFLAGS_OTHER)
    target_link_options(gtk4_lib INTERFACE
        ${GTK4_LDFLAGS_OTHER}
    )
endif ()

# Bundle the app's assets (stylesheet, and icons later) into a GResource.
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)

set(WORDSMITH_GRESOURCE_XML ${CMAKE_CURRENT_SOURCE_DIR}/wordsmith.gresource.xml)
set(WORDSMITH_RESOURCES_C   ${CMAKE_CURRENT_BINARY_DIR}/wordsmith-resources.c)

# Rebuild the generated source when any bundled file (or the manifest) changes.
# CONFIGURE_DEPENDS re-globs on reconfigure, so new icons get picked up.
file(GLOB WORDSMITH_GRESOURCE_DEPS CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/icons/*.svg)

# --manual-register: wordsmith-ui is a static library, so a resource object
# holding nothing but a constructor would be dropped from the final link.
# Calling wordsmith_resources_register_resource() from wordsmith-ui.c
# references a symbol in this object and forces it into the executable.
add_custom_command(
    OUTPUT ${WORDSMITH_RESOURCES_C}
    COMMAND ${GLIB_COMPILE_RESOURCES} --target=${WORDSMITH_RESOURCES_C}
            --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}
            --generate-source --manual-register --c-name wordsmith_resources
            ${WORDSMITH_GRESOURCE_XML}
    DEPENDS ${WORDSMITH_GRESOURCE_XML}
            ${CMAKE_CURRENT_SOURCE_DIR}/style.css
            ${WORDSMITH_GRESOURCE_DEPS}
    VERBATIM)

add_library(wordsmith-ui STATIC)

target_sources(wordsmith-ui PRIVATE ${WORDSMITH_RESOURCES_C})

target_sources(wordsmith-ui PUBLIC
    wordsmith-ui.h
)

target_sources(wordsmith-ui PRIVATE
    wordsmith-ui.c
    ui-state.c
    main-window.c
    menu-bar.c
    project-actions.c
    binder-panel.c
    editable-label.c
    editor-panel.c
    format-bar.c
    inspector-panel.c
    text-scale.c
    undo-stack.c
)

target_link_libraries(wordsmith-ui PRIVATE
    gtk4_lib
    wordsmith-core
)
