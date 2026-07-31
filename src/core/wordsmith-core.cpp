#include "wordsmith-core.hpp"

#ifndef WORDSMITH_VERSION_STRING
#define WORDSMITH_VERSION_STRING "0.0.0"
#endif

namespace wordsmith {

std::string version()
{
    return WORDSMITH_VERSION_STRING;
}

} // namespace wordsmith
