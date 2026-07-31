#pragma once

/* C bridge over the C++ core. The GTK UI is C, so everything it needs from
 * wordsmith-core crosses this header. */

#ifdef __cplusplus
extern "C" {
#endif

/* Borrowed, static storage. Do not free. */
const char* wordsmith_version(void);

#ifdef __cplusplus
}
#endif
