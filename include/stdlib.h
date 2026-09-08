#ifndef _CINDER_STDLIB_H
#define _CINDER_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767

void *malloc(size_t size);
void *calloc(size_t n, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

void exit(int status);
void abort(void);
int atexit(void (*fn)(void));

int atoi(const char *s);
long atol(const char *s);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double atof(const char *s);
double strtod(const char *s, char **end);

int abs(int n);
long labs(long n);

int rand(void);
void srand(unsigned int seed);

char *getenv(const char *name);
int system(const char *cmd);

void qsort(void *base, size_t n, size_t size,
           int (*cmp)(const void *, const void *));

#endif
