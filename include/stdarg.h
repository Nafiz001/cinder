// stdarg.h for cinder.
//
// On Windows every argument -- named or not, integer or floating -- occupies
// one 8-byte slot in the caller's outgoing area, and those slots are
// contiguous. That makes va_list a plain pointer and va_arg a pointer bump,
// which is why almost all of this is ordinary C.
//
// System V is much harder: arguments are split between integer and SSE
// registers with separate counters, so its va_list is a structure with two
// indices and two save areas, and va_arg has to branch on the argument's
// class. Only va_start needs compiler help here.

#ifndef _CINDER_STDARG_H
#define _CINDER_STDARG_H

typedef char *va_list;

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) (*(type *)((ap += 8) - 8))
#define va_end(ap) ((void)0)
#define va_copy(dst, src) ((dst) = (src))

#endif
