#include "wordsworth-core.hpp"

#ifndef WORDSWORTH_VERSION_STRING
#define WORDSWORTH_VERSION_STRING "0.0.0"
#endif

namespace wordsworth {

std::string version()
{
    return WORDSWORTH_VERSION_STRING;
}

} // namespace wordsworth
