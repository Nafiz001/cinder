// Scopes, shadowing, statics, and the two namespaces.
#include <stdio.h>

int counter(void) {
  // A static local lives for the whole program but is visible only here.
  static int n = 0;
  return ++n;
}

int x = 1;

struct stat { int a; };

int main(void) {
  printf("%d\n", x);
  {
    int x = 2;
    printf("%d\n", x);
    {
      int x = 3;
      printf("%d\n", x);
    }
    printf("%d\n", x);
  }
  printf("%d\n", x);

  // Deliberately sequenced. The order in which a call's arguments are
  // evaluated is unspecified in C, so writing counter() three times in one
  // argument list would compare cinder against gcc's choice rather than
  // against the language.
  int c1 = counter();
  int c2 = counter();
  int c3 = counter();
  printf("%d %d %d\n", c1, c2, c3);

  // Tags and ordinary identifiers are separate namespaces, which is what lets
  // a variable be called `stat` while `struct stat` also exists.
  struct stat stat;
  stat.a = 7;
  printf("%d\n", stat.a);

  for (int i = 0; i < 2; i++) {
    int i2 = i * 10;
    printf("%d %d\n", i, i2);
  }

  int shadow = 1;
  if (1) {
    int shadow = 2;
    printf("%d\n", shadow);
  }
  printf("%d\n", shadow);
  return 0;
}
