// stdio.h for cinder.
//
// These are declarations only. The definitions come from the platform's own C
// runtime at link time, which is why the prototypes have to match it exactly:
// cinder follows the Microsoft x64 ABI, so a call generated from this header
// reaches the real printf with its arguments in the right places.
//
// This file exists because MinGW's own stdio.h is written for gcc and is
// dense with __declspec, __attribute__ and __inline, none of which cinder
// implements. What is bundled here is the interface, not an implementation.

#ifndef _CINDER_STDIO_H
#define _CINDER_STDIO_H

#include <stddef.h>

typedef struct _iobuf FILE;

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int printf(const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int scanf(const char *fmt, ...);
int sscanf(const char *s, const char *fmt, ...);

// The v- forms take an already-started argument list. va_list is char* here,
// spelled out rather than included, so that stdio.h does not drag in stdarg.h.
int vprintf(const char *fmt, char *ap);
int vfprintf(FILE *stream, const char *fmt, char *ap);
int vsprintf(char *buf, const char *fmt, char *ap);
int vsnprintf(char *buf, size_t n, const char *fmt, char *ap);

int puts(const char *s);
int putchar(int c);
int getchar(void);
int fputs(const char *s, FILE *stream);
int fputc(int c, FILE *stream);
int fgetc(FILE *stream);
char *fgets(char *buf, int n, FILE *stream);

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
int fflush(FILE *stream);
size_t fread(void *ptr, size_t size, size_t n, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t n, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
int remove(const char *path);
int rename(const char *from, const char *to);

void perror(const char *s);

// On the Microsoft runtime stdin, stdout and stderr are not variables but a
// function returning an array of three streams. Spelling that out here is what
// lets fprintf(stderr, ...) work without cinder knowing anything special.
FILE *__acrt_iob_func(unsigned index);
#define stdin (__acrt_iob_func(0))
#define stdout (__acrt_iob_func(1))
#define stderr (__acrt_iob_func(2))

#endif
