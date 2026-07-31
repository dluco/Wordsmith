#include "wordsmith-core-c.h"

#include "wordsmith-core.hpp"

#include <string>

extern "C" const char* wordsmith_version(void)
{
    static const std::string cached = wordsmith::version();
    return cached.c_str();
}
