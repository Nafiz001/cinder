// Strings and the library functions cinder's headers declare.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
  char buf[64];
  strcpy(buf, "hello");
  printf("%s %d\n", buf, (int)strlen(buf));

  strcat(buf, " world");
  printf("%s %d\n", buf, (int)strlen(buf));

  printf("%d\n", strcmp("abc", "abc"));
  printf("%d\n", strcmp("abc", "abd") < 0);
  printf("%d\n", strncmp("abcdef", "abcxyz", 3));

  char *found = strchr(buf, 'w');
  printf("%s\n", found);

  memset(buf, 'x', 5);
  buf[5] = 0;
  printf("%s\n", buf);

  char dst[16];
  memcpy(dst, "copied", 7);
  printf("%s\n", dst);

  printf("%d\n", atoi("1234"));
  printf("%d\n", (int)strtol("ff", 0, 16));

  int *heap = malloc(sizeof(int) * 4);
  for (int i = 0; i < 4; i++) heap[i] = i * 5;
  for (int i = 0; i < 4; i++) printf("%d ", heap[i]);
  printf("\n");
  free(heap);

  // Escape sequences.
  printf("tab:\there\n");
  printf("quote:\"q\"\n");
  printf("backslash:\\\n");
  printf("octal:%c\n", '\101');
  printf("hex:%c\n", '\x42');
  printf("nul-in-middle:%d\n", (int)sizeof("a\0b"));

  char c = 'A';
  printf("%c %d\n", c, c);
  return 0;
}
