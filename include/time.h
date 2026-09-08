#ifndef _CINDER_TIME_H
#define _CINDER_TIME_H

#include <stddef.h>

typedef long long time_t;
typedef long long clock_t;

#define CLOCKS_PER_SEC 1000

time_t time(time_t *t);
clock_t clock(void);
double difftime(time_t end, time_t start);

#endif
