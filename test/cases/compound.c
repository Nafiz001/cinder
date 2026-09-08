// Compound literals.
#include <stdio.h>

struct P { int x; int y; };

int psum(struct P p) { return p.x + p.y; }

struct P *keep(struct P *p) { return p; }

int main(void) {
  struct P p = (struct P){3, 4};
  printf("%d %d\n", p.x, p.y);

  printf("%d\n", psum((struct P){5, 6}));

  // A compound literal is an object, so its address can be taken.
  struct P *pp = &(struct P){7, 8};
  printf("%d %d\n", pp->x, pp->y);

  int *a = (int[]){1, 2, 3, 4};
  printf("%d %d\n", a[0], a[3]);

  printf("%d\n", ((struct P){1, 2}).y);

  struct P arr[2] = {(struct P){1, 2}, (struct P){3, 4}};
  printf("%d %d\n", arr[0].x, arr[1].y);
  return 0;
}
