find_package(PkgConfig QUIET REQUIRED)

pkg_check_modules(GTK4 QUIET REQUIRED gtk4)

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

set(WORDSWORTH_GRESOURCE_XML ${CMAKE_CURRENT_SOURCE_DIR}/wordsworth.gresource.xml)
set(WORDSWORTH_RESOURCES_C   ${CMAKE_CURRENT_BINARY_DIR}/wordsworth-resources.c)

# Rebuild the generated source when any bundled file (or the manifest) changes.
# CONFIGURE_DEPENDS re-globs on reconfigure, so new icons get picked up.
file(GLOB WORDSWORTH_GRESOURCE_DEPS CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/icons/*.svg)

# --manual-register: wordsworth-ui is a static library, so a resource object
# holding nothing but a constructor would be dropped from the final link.
# Calling wordsworth_resources_register_resource() from wordsworth-ui.c
# references a symbol in this object and forces it into the executable.
add_custom_command(
    OUTPUT ${WORDSWORTH_RESOURCES_C}
    COMMAND ${GLIB_COMPILE_RESOURCES} --target=${WORDSWORTH_RESOURCES_C}
            --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}
            --generate-source --manual-register --c-name wordsworth_resources
            ${WORDSWORTH_GRESOURCE_XML}
    DEPENDS ${WORDSWORTH_GRESOURCE_XML}
            ${CMAKE_CURRENT_SOURCE_DIR}/style.css
            ${WORDSWORTH_GRESOURCE_DEPS}
    VERBATIM)

add_library(wordsworth-ui STATIC)

target_sources(wordsworth-ui PRIVATE ${WORDSWORTH_RESOURCES_C})

target_sources(wordsworth-ui PUBLIC
    wordsworth-ui.h
)

target_sources(wordsworth-ui PRIVATE
    wordsworth-ui.c
    ui-state.c
    main-window.c
    menu-bar.c
    binder-panel.c
    editor-panel.c
    inspector-panel.c
)

target_link_libraries(wordsworth-ui PRIVATE
    gtk4_lib
    wordsworth-core
)
