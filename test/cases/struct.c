// Structs, unions, enums and typedefs.
#include <stdio.h>

struct Point { int x; int y; };

struct Nested { struct Point a; struct Point b; int tag; };

union Value { int i; char bytes[4]; };

enum Color { RED, GREEN, BLUE = 10, PURPLE };

typedef struct Point Pt;
typedef int Integer;
typedef struct { int w; int h; } Size;

struct Point make(int x, int y) {
  struct Point p;
  p.x = x;
  p.y = y;
  return p;
}

// Larger than 8 bytes, so it travels by hidden pointer.
struct Nested make_nested(void) {
  struct Nested n;
  n.a.x = 1; n.a.y = 2;
  n.b.x = 3; n.b.y = 4;
  n.tag = 99;
  return n;
}

int point_sum(struct Point p) { return p.x + p.y; }
int nested_sum(struct Nested n) { return n.a.x + n.a.y + n.b.x + n.b.y + n.tag; }

int main(void) {
  struct Point p;
  p.x = 3;
  p.y = 4;
  printf("%d %d\n", p.x, p.y);

  struct Point *pp = &p;
  printf("%d %d\n", pp->x, pp->y);
  pp->x = 30;
  printf("%d\n", p.x);

  struct Point q = p;
  printf("%d %d\n", q.x, q.y);
  q.x = 100;
  printf("%d %d\n", p.x, q.x);

  struct Nested n;
  n.a.x = 1; n.a.y = 2; n.b.x = 3; n.b.y = 4; n.tag = 5;
  printf("%d %d %d\n", n.a.x, n.b.y, n.tag);

  union Value v;
  v.i = 0;
  v.bytes[0] = 1;
  printf("%d\n", v.i);

  printf("%d %d %d %d\n", RED, GREEN, BLUE, PURPLE);
  enum Color c = BLUE;
  printf("%d\n", c);

  Pt tp;
  tp.x = 7; tp.y = 8;
  printf("%d %d\n", tp.x, tp.y);

  Integer ti = 42;
  printf("%d\n", ti);

  Size s;
  s.w = 11; s.h = 22;
  printf("%d %d\n", s.w, s.h);

  struct Point m = make(5, 6);
  printf("%d %d\n", m.x, m.y);
  printf("%d\n", point_sum(m));

  struct Nested bn = make_nested();
  printf("%d %d %d\n", bn.a.x, bn.b.y, bn.tag);
  printf("%d\n", nested_sum(bn));

  struct Point arr[3];
  for (int i = 0; i < 3; i++) { arr[i].x = i; arr[i].y = i * 2; }
  printf("%d %d %d\n", arr[0].x, arr[1].y, arr[2].x);
  return 0;
}
