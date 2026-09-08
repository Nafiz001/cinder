#ifndef _CINDER_STDDEF_H
#define _CINDER_STDDEF_H

typedef unsigned long long size_t;
typedef long long ptrdiff_t;

#define NULL ((void *)0)
#define offsetof(type, member) ((size_t) & (((type *)0)->member))

#endif
