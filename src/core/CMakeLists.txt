# Enchant reads whatever dictionaries the system has (hunspell, in practice)
# and picks one for a language tag. Its C header pulls in nothing but
# <stdint.h>, so it does not cost this layer the "no GLib" rule the rest of it
# keeps. A machine with no dictionary installed still builds and runs; see
# spelling.hpp for what happens then.
find_package(PkgConfig QUIET REQUIRED)
pkg_check_modules(ENCHANT QUIET REQUIRED IMPORTED_TARGET enchant-2)

add_library(wordsmith-core STATIC
    wordsmith-core.cpp
    wordsmith-core-c.cpp
    markup.cpp
    markup-c.cpp
    spelling.cpp
    spelling-c.cpp
    project.cpp
    project-c.cpp
    preferences.cpp
    preferences-c.cpp
    session.cpp
    session-c.cpp
    safe-write.cpp
    snapshots.cpp
    yaml.cpp
    frontmatter.cpp
    frontmatter-c.cpp
)

set_target_properties(wordsmith-core PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_sources(wordsmith-core PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ..
    FILES
    wordsmith-core.hpp
    wordsmith-core-c.h
    markup.hpp
    markup-c.h
    spelling.hpp
    spelling-c.h
    project.hpp
    project-c.h
    preferences.hpp
    preferences-c.h
    session.hpp
    session-c.h
    safe-write.hpp
    snapshots.hpp
    yaml.hpp
    frontmatter.hpp
    frontmatter-c.h
)

# Both private: enchant.h is included by spelling.cpp alone, and nothing in a
# public header names a type from either library. CMake still hands the link
# flags on to whoever links this static library, which is what the executable
# and the test binaries need.
target_link_libraries(wordsmith-core PRIVATE
    argo
    PkgConfig::ENCHANT
)

target_compile_definitions(wordsmith-core PRIVATE
    WORDSMITH_VERSION_STRING="${PROJECT_VERSION}"
)
