// Type sizes. Windows is LLP64, so long is 4 bytes -- the single most likely
// place for a compiler to disagree with its platform.
#include <stdio.h>

struct S { char a; int b; char c; };
struct Packed { char a; char b; };
union U { int i; char c[8]; };

int main(void) {
  printf("%d\n", (int)sizeof(char));
  printf("%d\n", (int)sizeof(short));
  printf("%d\n", (int)sizeof(int));
  printf("%d\n", (int)sizeof(long));
  printf("%d\n", (int)sizeof(long long));
  printf("%d\n", (int)sizeof(void *));
  printf("%d\n", (int)sizeof(float));
  printf("%d\n", (int)sizeof(double));
  printf("%d\n", (int)sizeof(int *));
  printf("%d\n", (int)sizeof(int[10]));
  printf("%d\n", (int)sizeof(struct S));
  printf("%d\n", (int)sizeof(struct Packed));
  printf("%d\n", (int)sizeof(union U));

  // A character constant has type int, which is why this is 4 and not 1.
  printf("%d\n", (int)sizeof('a'));
  printf("%d\n", (int)sizeof(1));
  printf("%d\n", (int)sizeof(1L));
  printf("%d\n", (int)sizeof(1LL));
  printf("%d\n", (int)sizeof(1.0));
  printf("%d\n", (int)sizeof(1.0f));

  int arr[7];
  printf("%d\n", (int)sizeof(arr));
  printf("%d\n", (int)sizeof(arr) / (int)sizeof(arr[0]));
  return 0;
}
