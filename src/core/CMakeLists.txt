add_library(wordsworth-core STATIC
    wordsworth-core.cpp
    wordsworth-core-c.cpp
)

set_target_properties(wordsworth-core PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_sources(wordsworth-core PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ..
    FILES
    wordsworth-core.hpp
    wordsworth-core-c.h
)

target_compile_definitions(wordsworth-core PRIVATE
    WORDSWORTH_VERSION_STRING="${PROJECT_VERSION}"
)
