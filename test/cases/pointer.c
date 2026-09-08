// Pointers and arrays.
#include <stdio.h>

int g = 7;

void inc(int *p) { (*p)++; }

void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }

int main(void) {
  int x = 3;
  int *p = &x;
  printf("%d\n", *p);
  *p = 5;
  printf("%d\n", x);

  inc(&x);
  printf("%d\n", x);

  int a = 1, b = 2;
  swap(&a, &b);
  printf("%d %d\n", a, b);

  int arr[5];
  for (int i = 0; i < 5; i++) arr[i] = i * i;
  for (int i = 0; i < 5; i++) printf("%d ", arr[i]);
  printf("\n");

  // Pointer arithmetic scales by the element size.
  int *q = arr;
  printf("%d %d %d\n", *q, *(q + 2), q[3]);
  printf("%d\n", (int)(&arr[4] - &arr[1]));

  // a[i] is *(a+i), which is why this works.
  printf("%d\n", 2[arr]);

  q = arr + 2;
  printf("%d\n", *q);
  q++;
  printf("%d\n", *q);
  q--;
  printf("%d\n", *q);

  int **pp = &q;
  printf("%d\n", **pp);

  int two[3][4];
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 4; j++)
      two[i][j] = i * 10 + j;
  printf("%d %d %d\n", two[0][0], two[1][2], two[2][3]);
  printf("%d\n", (int)sizeof(two));
  printf("%d\n", (int)sizeof(two[0]));

  printf("%d\n", g);
  int *gp = &g;
  *gp = 9;
  printf("%d\n", g);

  // A null pointer is false, a real one is true.
  int *null = 0;
  printf("%d %d\n", null == 0, p != 0);
  return 0;
}
