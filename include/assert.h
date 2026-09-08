#ifndef _CINDER_ASSERT_H
#define _CINDER_ASSERT_H

#include <stdio.h>
#include <stdlib.h>

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
// The stringized expression is what makes a failed assertion readable, and it
// is the reason assert has to be a macro rather than a function.
#define assert(expr)                                                           \
  ((expr) ? (void)0                                                            \
          : (printf("%s:%d: assertion failed: %s\n", __FILE__, __LINE__,       \
                    #expr),                                                    \
             abort()))
#endif

#endif
