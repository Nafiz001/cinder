// The preprocessor.
#include <stdio.h>

#define ANSWER 42
#define GREETING "hello"
#define SQUARE(x) ((x) * (x))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CONCAT(a, b) a##b
#define STR(x) #x
#define XSTR(x) STR(x)
#define EMPTY
#define LOG(fmt, ...) printf("log: " fmt "\n", ##__VA_ARGS__)

// A macro that mentions itself must expand once and stop.
#define recur recur_done
#define nested SQUARE(3)

#define MULTI(a, b)                                                            \
  do {                                                                         \
    printf("%d %d\n", a, b);                                                   \
  } while (0)

int concatvar = 5;

int main(void) {
  printf("%d\n", ANSWER);
  printf("%s\n", GREETING);
  printf("%d\n", SQUARE(4));
  // The parenthesisation in SQUARE is what makes this 49 and not 13.
  printf("%d\n", SQUARE(3 + 4));
  printf("%d\n", MAX(3, 7));
  printf("%d\n", MAX(SQUARE(3), 8));
  printf("%d\n", CONCAT(concat, var));
  printf("%s\n", STR(raw text));
  printf("%s\n", XSTR(ANSWER));
  printf("%d\n", nested);
  EMPTY
  printf("empty macro ok\n");

  LOG("no args");
  LOG("with %d", 7);

  MULTI(1, 2);

#ifdef ANSWER
  printf("ifdef ok\n");
#endif

#ifndef NOT_DEFINED
  printf("ifndef ok\n");
#endif

#if ANSWER == 42
  printf("if-equal ok\n");
#else
  printf("wrong branch\n");
#endif

#if ANSWER > 100
  printf("wrong\n");
#elif ANSWER > 40
  printf("elif ok\n");
#else
  printf("wrong\n");
#endif

#if defined(ANSWER) && !defined(NOT_DEFINED)
  printf("defined ok\n");
#endif

// An undefined identifier in #if is zero, not an error.
#if UNDEFINED_THING
  printf("wrong\n");
#else
  printf("undefined is zero\n");
#endif

#if 1
#if 0
  printf("wrong\n");
#else
  printf("nested conditional ok\n");
#endif
#endif

#undef ANSWER
#ifndef ANSWER
  printf("undef ok\n");
#endif

  printf("%d\n", __LINE__);
  return 0;
}
