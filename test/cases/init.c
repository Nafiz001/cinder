// Initialisers, globals and string literals.
#include <stdio.h>

int gi = 42;
int garr[5] = {1, 2, 3};
int gzero[4];
char gstr[] = "hello";
char *gptr = "world";
int *gaddr = &gi;

struct P { int x; int y; };
struct P gp = {3, 4};
struct P gparr[2] = {{1, 2}, {3, 4}};

int gmatrix[2][3] = {{1, 2, 3}, {4, 5, 6}};
int gflat[2][3] = {1, 2, 3, 4, 5, 6};

int main(void) {
  printf("%d\n", gi);
  for (int i = 0; i < 5; i++) printf("%d ", garr[i]);
  printf("\n");
  for (int i = 0; i < 4; i++) printf("%d ", gzero[i]);
  printf("\n");
  printf("%s %s\n", gstr, gptr);
  printf("%d\n", *gaddr);
  printf("%d %d\n", gp.x, gp.y);
  printf("%d %d\n", gparr[1].x, gparr[1].y);
  printf("%d %d\n", gmatrix[1][2], gflat[1][2]);

  int a[5] = {1, 2, 3};
  for (int i = 0; i < 5; i++) printf("%d ", a[i]);
  printf("\n");

  int b[] = {9, 8, 7};
  printf("%d %d\n", (int)(sizeof(b) / sizeof(b[0])), b[1]);

  char s[] = "abc";
  printf("%d %s\n", (int)sizeof(s), s);

  struct P lp = {5, 6};
  printf("%d %d\n", lp.x, lp.y);

  struct P lparr[2] = {{1, 2}, {3, 4}};
  printf("%d %d\n", lparr[0].y, lparr[1].x);

  int m[2][3] = {{1, 2, 3}, {4, 5, 6}};
  printf("%d %d\n", m[0][2], m[1][0]);

  int partial[10] = {1};
  int sum = 0;
  for (int i = 0; i < 10; i++) sum += partial[i];
  printf("%d\n", sum);

  // Designated initialisers.
  int d[5] = {[1] = 10, [3] = 30};
  for (int i = 0; i < 5; i++) printf("%d ", d[i]);
  printf("\n");

  struct P dp = {.y = 9, .x = 8};
  printf("%d %d\n", dp.x, dp.y);

  const char *msg = "literal";
  printf("%s %c\n", msg, msg[2]);
  printf("%d\n", (int)sizeof("hello"));
  return 0;
}
