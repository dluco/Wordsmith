#include "wordsworth-core-c.h"

#include "wordsworth-core.hpp"

#include <string>

extern "C" const char* wordsworth_version(void)
{
    static const std::string cached = wordsworth::version();
    return cached.c_str();
}
