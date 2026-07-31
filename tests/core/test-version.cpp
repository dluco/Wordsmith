#include "core/wordsworth-core-c.h"
#include "core/wordsworth-core.hpp"

#include <cassert>
#include <cstring>
#include <string>

/* Exercises the C bridge as well as the C++ entry point, so a mismatch in the
 * boundary shows up here rather than in the UI. */
int main()
{
    const std::string from_cxx = wordsworth::version();
    assert(!from_cxx.empty());

    const char* from_c = wordsworth_version();
    assert(from_c != nullptr);
    assert(from_cxx == from_c);

    /* The cached bridge string must stay valid across calls. */
    assert(wordsworth_version() == from_c);

    return 0;
}
