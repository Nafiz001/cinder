// Control flow.
#include <stdio.h>

int main(void) {
  for (int i = 0; i < 3; i++)
    printf("for %d\n", i);

  int i = 0;
  while (i < 3) { printf("while %d\n", i); i++; }

  i = 0;
  do { printf("do %d\n", i); i++; } while (i < 3);

  for (i = 0; i < 6; i++) {
    if (i == 2) continue;
    if (i == 4) break;
    printf("skip %d\n", i);
  }

  for (i = 0; i < 5; i++) {
    switch (i) {
    case 0: printf("zero\n"); break;
    case 1:
    case 2: printf("one or two: %d\n", i); break;
    default: printf("many: %d\n", i); break;
    }
  }

  // Fallthrough is deliberate here.
  switch (1) {
  case 1: printf("fall 1\n");
  case 2: printf("fall 2\n"); break;
  case 3: printf("not reached\n");
  }

  i = 0;
loop:
  if (i < 3) { printf("goto %d\n", i); i++; goto loop; }

  // Nested loops: break leaves only the inner one.
  for (i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      if (j == 1) break;
      printf("nested %d %d\n", i, j);
    }
  }

  // A switch inside a loop: break belongs to the switch.
  for (i = 0; i < 3; i++) {
    switch (i) {
    case 1: break;
    default: printf("sw-in-loop %d\n", i);
    }
  }
  return 0;
}
