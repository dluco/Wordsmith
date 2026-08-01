add_library(wordsmith-core STATIC
    wordsmith-core.cpp
    wordsmith-core-c.cpp
    markup.cpp
    markup-c.cpp
    project.cpp
    project-c.cpp
    preferences.cpp
    preferences-c.cpp
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
    project.hpp
    project-c.h
    preferences.hpp
    preferences-c.h
    safe-write.hpp
    snapshots.hpp
    yaml.hpp
    frontmatter.hpp
    frontmatter-c.h
)

target_link_libraries(wordsmith-core PRIVATE
    argo
)

target_compile_definitions(wordsmith-core PRIVATE
    WORDSMITH_VERSION_STRING="${PROJECT_VERSION}"
)
